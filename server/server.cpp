#include "server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)
#include <sys/event.h>
#elif defined(__linux__)
#include <sys/epoll.h>
#else
#error "Tavern server requires kqueue or epoll"
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tavern/database.hpp>
#include <unordered_map>
#include <utility>
#include <vector>

#include "protocol.hpp"

namespace {

using Clock = std::chrono::steady_clock;

volatile std::sig_atomic_t stop_requested = 0;

// Signal handlers only set an async-signal-safe flag. The event loop performs
// descriptor cleanup and database close in normal control flow.
void request_stop(int) { stop_requested = 1; }

void install_signal_handlers() {
  struct sigaction stop_action{};
  stop_action.sa_handler = request_stop;
  sigemptyset(&stop_action.sa_mask);
  sigaction(SIGINT, &stop_action, nullptr);
  sigaction(SIGTERM, &stop_action, nullptr);

  struct sigaction pipe_action{};
  pipe_action.sa_handler = SIG_IGN;
  sigemptyset(&pipe_action.sa_mask);
  sigaction(SIGPIPE, &pipe_action, nullptr);
}

class FileDescriptor {
 public:
  explicit FileDescriptor(int descriptor = -1) : descriptor_(descriptor) {}
  ~FileDescriptor() {
    if (descriptor_ >= 0) close(descriptor_);
  }
  FileDescriptor(const FileDescriptor&) = delete;
  auto operator=(const FileDescriptor&) -> FileDescriptor& = delete;
  FileDescriptor(FileDescriptor&& other) noexcept
      : descriptor_(other.release()) {}
  auto operator=(FileDescriptor&& other) noexcept -> FileDescriptor& {
    if (this != &other) {
      if (descriptor_ >= 0) close(descriptor_);
      descriptor_ = other.release();
    }
    return *this;
  }
  auto get() const -> int { return descriptor_; }
  auto release() -> int { return std::exchange(descriptor_, -1); }

 private:
  int descriptor_;
};

struct Options {
  std::string database;
  std::string host = "127.0.0.1";
  std::uint16_t port = 7373;
};

struct ReadyEvent {
  int descriptor;
  bool readable;
  bool writable;
  bool error;
};

class EventQueue {
 public:
  explicit EventQueue(int descriptor) : descriptor_(descriptor) {}
  ~EventQueue() {
    if (descriptor_ >= 0) close(descriptor_);
  }
  EventQueue(const EventQueue&) = delete;
  auto operator=(const EventQueue&) -> EventQueue& = delete;
  EventQueue(EventQueue&& other) noexcept
      : descriptor_(std::exchange(other.descriptor_, -1)) {}

  static auto create() -> std::expected<EventQueue, std::string_view> {
    // kqueue represents read and write filters separately; epoll stores the
    // desired masks together. The wrapper exposes only the common readiness
    // operations needed by the loop and does not change either API's level-
    // triggered behavior.
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)
    const int descriptor = kqueue();
#else
    const int descriptor = epoll_create1(EPOLL_CLOEXEC);
#endif
    if (descriptor < 0) return std::unexpected("event queue failure");
    return EventQueue(descriptor);
  }

  auto add_listener(int descriptor) -> bool { return add_client(descriptor); }

  auto add_client(int descriptor) -> bool {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)
    return change(descriptor, EVFILT_READ, EV_ADD | EV_ENABLE);
#else
    epoll_event event{};
    event.events = EPOLLIN | EPOLLRDHUP;
    event.data.fd = descriptor;
    return epoll_ctl(descriptor_, EPOLL_CTL_ADD, descriptor, &event) == 0;
#endif
  }

  auto set_writable(int descriptor, bool enabled) -> bool {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)
    return change(descriptor, EVFILT_WRITE,
                  EV_ADD | (enabled ? EV_ENABLE : EV_DISABLE));
#else
    epoll_event event{};
    event.events = EPOLLIN | EPOLLRDHUP;
    if (enabled) event.events |= EPOLLOUT;
    event.data.fd = descriptor;
    return epoll_ctl(descriptor_, EPOLL_CTL_MOD, descriptor, &event) == 0;
#endif
  }

  auto wait(std::span<ReadyEvent> ready, std::chrono::milliseconds timeout)
      -> int {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)
    std::array<struct kevent, 64> events{};
    const auto bounded = std::min<std::size_t>(ready.size(), events.size());
    const timespec wait_time{
        .tv_sec = static_cast<time_t>(timeout.count() / 1000),
        .tv_nsec = static_cast<long>((timeout.count() % 1000) * 1000000),
    };
    const int count = kevent(descriptor_, nullptr, 0, events.data(),
                             static_cast<int>(bounded), &wait_time);
    if (count <= 0) return count;
    for (int index = 0; index < count; ++index) {
      const struct kevent& event = events[static_cast<std::size_t>(index)];
      ready[static_cast<std::size_t>(index)] = {
          .descriptor = static_cast<int>(event.ident),
          .readable = event.filter == EVFILT_READ,
          .writable = event.filter == EVFILT_WRITE,
          .error = (event.flags & EV_ERROR) != 0,
      };
    }
    return count;
#else
    std::array<epoll_event, 64> events{};
    const auto bounded = std::min<std::size_t>(ready.size(), events.size());
    const int count =
        epoll_wait(descriptor_, events.data(), static_cast<int>(bounded),
                   static_cast<int>(std::min<std::int64_t>(
                       timeout.count(), std::numeric_limits<int>::max())));
    if (count <= 0) return count;
    for (int index = 0; index < count; ++index) {
      const epoll_event& event = events[static_cast<std::size_t>(index)];
      ready[static_cast<std::size_t>(index)] = {
          .descriptor = event.data.fd,
          .readable = (event.events & (EPOLLIN | EPOLLRDHUP)) != 0,
          .writable = (event.events & EPOLLOUT) != 0,
          .error = (event.events & (EPOLLERR | EPOLLHUP)) != 0,
      };
    }
    return count;
#endif
  }

 private:
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)
  auto change(int descriptor, int filter, std::uint16_t flags) -> bool {
    struct kevent event{};
    EV_SET(&event, static_cast<uintptr_t>(descriptor), filter, flags, 0, 0,
           nullptr);
    return kevent(descriptor_, &event, 1, nullptr, 0, nullptr) == 0;
  }
#endif

  int descriptor_;
};

struct Connection {
  // At most one completed request awaits execution. `input` retains subsequent
  // bytes, `output` retains unsent complete responses, and `discarding` drops
  // an overlong frame through its LF delimiter so framing can resume safely.
  int descriptor;
  std::string request;
  std::string input;
  std::string output;
  std::optional<std::string> ready_request;
  bool discarding = false;
  bool ready_request_too_large = false;
  bool peer_closed = false;
  Clock::time_point last_activity = Clock::now();
};

constexpr auto idle_timeout = std::chrono::seconds(30);
constexpr std::size_t maximum_input_bytes = 8192;
constexpr std::size_t maximum_responses_per_turn = 16;
constexpr std::size_t maximum_pending_output =
    maximum_responses_per_turn * tavern::protocol::maximum_response_size;
constexpr std::size_t maximum_write_bytes_per_turn =
    tavern::protocol::maximum_response_size;
constexpr std::string_view allocation_failure = "ERR RESOURCE_EXHAUSTED\n";
constexpr std::string_view request_too_large = "ERR REQUEST_TOO_LARGE\n";

auto database_open_diagnostic(tavern::Error error) -> std::string_view {
  switch (error) {
    case tavern::Error::database_locked:
      return "database is locked by another Tavern process";
    case tavern::Error::invalid_page:
    case tavern::Error::invalid_metadata:
      return "database is corrupt or uses an unsupported file format";
    case tavern::Error::recovery_required:
      return "database recovery is required; stop, back up, and inspect the "
             "database";
    case tavern::Error::io_failure:
      return "cannot read database";
    default:
      return "cannot open database";
  }
}

void usage() {
  std::cerr << "usage: tavern-server --database <path> [--host <ipv4>] [--port "
               "<port>]\n";
}

auto parse_port(std::string_view value)
    -> std::expected<std::uint16_t, std::string_view> {
  if (value.empty()) return std::unexpected("invalid port");
  unsigned long parsed = 0;
  for (const char character : value) {
    if (character < '0' || character > '9')
      return std::unexpected("invalid port");
    const unsigned long digit = static_cast<unsigned long>(character - '0');
    if (parsed > (std::numeric_limits<std::uint16_t>::max() - digit) / 10)
      return std::unexpected("invalid port");
    parsed = parsed * 10 + digit;
  }
  return static_cast<std::uint16_t>(parsed);
}

auto parse_options(int argc, char** argv)
    -> std::expected<Options, std::string_view> {
  Options options;
  for (int index = 1; index < argc; index += 2) {
    if (index + 1 >= argc) return std::unexpected("missing option value");
    const std::string_view name(argv[index]);
    const std::string_view value(argv[index + 1]);
    if (name == "--database") {
      if (!options.database.empty())
        return std::unexpected("duplicate database");
      options.database = value;
    } else if (name == "--host") {
      options.host = value;
    } else if (name == "--port") {
      const auto port = parse_port(value);
      if (!port) return std::unexpected(port.error());
      options.port = *port;
    } else {
      return std::unexpected("unknown option");
    }
  }
  if (options.database.empty()) return std::unexpected("missing database");
  return options;
}

auto make_nonblocking(int descriptor) -> bool {
  const int flags = fcntl(descriptor, F_GETFL, 0);
  return flags >= 0 && fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

auto listen_socket(const Options& options, std::string& address,
                   std::uint16_t& port)
    -> std::expected<FileDescriptor, std::string_view> {
  sockaddr_in endpoint{};
  endpoint.sin_family = AF_INET;
  endpoint.sin_port = htons(options.port);
  if (options.host.find(':') != std::string::npos)
    return std::unexpected("IPv6 hosts are unsupported");
  if (inet_pton(AF_INET, options.host.c_str(), &endpoint.sin_addr) != 1)
    return std::unexpected("invalid IPv4 host");
  FileDescriptor socket_descriptor(socket(AF_INET, SOCK_STREAM, 0));
  if (socket_descriptor.get() < 0) return std::unexpected("socket failure");
  int enabled = 1;
  if (setsockopt(socket_descriptor.get(), SOL_SOCKET, SO_REUSEADDR, &enabled,
                 sizeof(enabled)) < 0 ||
      !make_nonblocking(socket_descriptor.get()) ||
      bind(socket_descriptor.get(),
           reinterpret_cast<const sockaddr*>(&endpoint),
           sizeof(endpoint)) < 0 ||
      listen(socket_descriptor.get(), SOMAXCONN) < 0) {
    return std::unexpected("listen failure");
  }
  socklen_t size = sizeof(endpoint);
  if (getsockname(socket_descriptor.get(),
                  reinterpret_cast<sockaddr*>(&endpoint), &size) < 0)
    return std::unexpected("listen failure");
  char host[INET_ADDRSTRLEN];
  if (inet_ntop(AF_INET, &endpoint.sin_addr, host, sizeof(host)) == nullptr)
    return std::unexpected("listen failure");
  address = host;
  port = ntohs(endpoint.sin_port);
  return socket_descriptor;
}

auto output_has_space(const Connection& connection) -> bool {
  return connection.output.size() <=
         maximum_pending_output - tavern::protocol::maximum_response_size;
}

auto queue_ready_response(Connection& connection, tavern::Database& database)
    -> bool {
  if (!connection.ready_request && !connection.ready_request_too_large)
    return true;
  if (!output_has_space(connection)) return false;
  if (connection.ready_request_too_large) {
    connection.output.append(request_too_large);
    connection.ready_request_too_large = false;
    return true;
  }
  // Commands are executed here, on the single event-loop thread. Connections
  // are multiplexed concurrently for I/O, but database operations never run in
  // parallel and a slow mutation delays subsequent ready requests.
  const auto response =
      tavern::protocol::execute_command(database, *connection.ready_request);
  connection.ready_request.reset();
  connection.output.append(response ? std::string_view(*response)
                                    : allocation_failure);
  return true;
}

auto process_input(Connection& connection, tavern::Database& database) -> bool {
  std::size_t offset = 0;
  std::size_t responses = 0;
  // Bound completed frames per readiness turn. Together with output limits this
  // prevents one pipelining client from monopolizing parsing, command
  // execution, or queued response memory.
  while (offset < connection.input.size() &&
         responses < maximum_responses_per_turn) {
    if (!queue_ready_response(connection, database)) break;
    if (connection.ready_request || connection.ready_request_too_large) break;

    const char character = connection.input[offset++];
    if (connection.discarding) {
      if (character == '\n') {
        connection.discarding = false;
        connection.ready_request_too_large = true;
        ++responses;
      }
      continue;
    }
    if (character != '\n') {
      connection.request.push_back(character);
      if (connection.request.size() >
          tavern::protocol::maximum_request_size + 1) {
        connection.request.clear();
        connection.discarding = true;
      }
      continue;
    }

    if (!connection.request.empty() && connection.request.back() == '\r')
      connection.request.pop_back();
    if (connection.request.size() > tavern::protocol::maximum_request_size) {
      connection.request.clear();
      connection.ready_request_too_large = true;
    } else {
      connection.ready_request = std::move(connection.request);
      connection.request.clear();
      connection.request.reserve(tavern::protocol::maximum_request_size + 1);
    }
    ++responses;
  }
  connection.input.erase(0, offset);
  static_cast<void>(queue_ready_response(connection, database));
  return true;
}

auto read_client(Connection& connection, tavern::Database& database) -> bool {
  if (!process_input(connection, database)) return true;
  if (connection.ready_request || connection.ready_request_too_large ||
      connection.input.size() == maximum_input_bytes)
    return true;

  // One nonblocking receive per readiness event bounds work. The input cap and
  // overlong-frame discard state bound per-connection request memory.
  std::array<char, maximum_input_bytes> buffer{};
  const std::size_t capacity = maximum_input_bytes - connection.input.size();
  const auto received = recv(connection.descriptor, buffer.data(), capacity, 0);
  if (received > 0) {
    connection.input.append(buffer.data(), static_cast<std::size_t>(received));
    connection.last_activity = Clock::now();
    return process_input(connection, database);
  }
  if (received == 0) {
    connection.peer_closed = true;
    return true;
  }
  if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return true;
  return false;
}

auto write_client(Connection& connection) -> bool {
  std::size_t written_this_turn = 0;
  // A send may consume only a prefix. Keep the remainder and enable writable
  // readiness until drained; the per-turn byte budget preserves fairness under
  // backpressure.
  while (!connection.output.empty() &&
         written_this_turn < maximum_write_bytes_per_turn) {
    const std::size_t remaining =
        maximum_write_bytes_per_turn - written_this_turn;
    const std::size_t size = std::min(connection.output.size(), remaining);
    const auto written =
        send(connection.descriptor, connection.output.data(), size, 0);
    if (written > 0) {
      const auto count = static_cast<std::size_t>(written);
      connection.output.erase(0, count);
      written_this_turn += count;
      connection.last_activity = Clock::now();
      continue;
    }
    if (written < 0 && errno == EINTR) continue;
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
    return false;
  }
  return true;
}

auto accept_clients(int listener, EventQueue& queue,
                    std::unordered_map<int, Connection>& clients) -> bool {
  // Accept in bounded batches for the same reason request and write work are
  // bounded: a connection flood must not starve existing clients.
  constexpr std::size_t maximum_accepts_per_turn = 32;
  for (std::size_t count = 0; count < maximum_accepts_per_turn; ++count) {
    const int descriptor = accept(listener, nullptr, nullptr);
    if (descriptor < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
      return false;
    }
    if (!make_nonblocking(descriptor) || !queue.add_client(descriptor)) {
      close(descriptor);
      continue;
    }
    Connection connection{};
    connection.descriptor = descriptor;
    connection.request.reserve(tavern::protocol::maximum_request_size + 1);
    connection.input.reserve(maximum_input_bytes);
    clients.emplace(descriptor, std::move(connection));
  }
  return true;
}

void close_client(std::unordered_map<int, Connection>& clients,
                  int descriptor) {
  const auto found = clients.find(descriptor);
  if (found == clients.end()) return;
  // Closing removes the descriptor from kqueue/epoll; erase releases buffered
  // request/response memory. No protocol-level close frame is defined.
  close(found->second.descriptor);
  clients.erase(found);
}

auto event_timeout(const std::unordered_map<int, Connection>& clients)
    -> std::chrono::milliseconds {
  auto timeout =
      std::chrono::duration_cast<std::chrono::milliseconds>(idle_timeout);
  const auto now = Clock::now();
  for (const auto& [descriptor, connection] : clients) {
    static_cast<void>(descriptor);
    const auto deadline = connection.last_activity + idle_timeout;
    if (deadline <= now) return std::chrono::milliseconds::zero();
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    timeout = std::min(timeout, remaining);
  }
  return timeout;
}

void close_idle_clients(std::unordered_map<int, Connection>& clients) {
  // Activity is updated only by successful socket I/O. Idle peers are dropped
  // without attempting to complete partial frames or buffered requests.
  const auto now = Clock::now();
  std::vector<int> expired;
  expired.reserve(clients.size());
  for (const auto& [descriptor, connection] : clients) {
    if (now - connection.last_activity >= idle_timeout)
      expired.push_back(descriptor);
  }
  for (const int descriptor : expired) close_client(clients, descriptor);
}

}  // namespace

auto tavern::server::run(int argc, char** argv) -> int {
  const auto options = parse_options(argc, argv);
  if (!options) {
    std::cerr << "error: " << options.error() << '\n';
    usage();
    return 1;
  }
  auto database = tavern::Database::open(options->database);
  if (!database) {
    std::error_code filesystem_error;
    const bool exists =
        std::filesystem::exists(options->database, filesystem_error);
    if (!exists && !filesystem_error) {
      std::cerr << "database file does not exist\n";
    } else {
      std::cerr << database_open_diagnostic(database.error()) << '\n';
    }
    return 1;
  }
  auto mutable_database = std::move(*database);
  std::string address;
  std::uint16_t port = 0;
  const auto listening = listen_socket(*options, address, port);
  if (!listening) {
    std::cerr << "failed to listen: " << listening.error() << '\n';
    mutable_database.close();
    return 1;
  }
  auto queue = EventQueue::create();
  if (!queue || !queue->add_listener(listening->get())) {
    std::cerr << "failed to create event queue\n";
    mutable_database.close();
    return 1;
  }

  install_signal_handlers();
  std::cout << "listening on " << address << ':' << port << '\n' << std::flush;
  EventQueue event_queue = std::move(*queue);
  std::unordered_map<int, Connection> clients;
  std::array<ReadyEvent, 64> ready{};
  bool loop_failed = false;
  // Shutdown stops accepting and closes clients after leaving the loop. Pending
  // responses are not drained, so graceful here means orderly local resource
  // cleanup rather than a delivery guarantee to every peer.
  while (!stop_requested) {
    const int count = event_queue.wait(ready, event_timeout(clients));
    if (count < 0) {
      if (errno == EINTR) continue;
      std::cerr << "event wait failure: " << std::strerror(errno) << '\n';
      loop_failed = true;
      break;
    }
    if (count > 0) {
      for (int index = 0; index < count; ++index) {
        const ReadyEvent event = ready[static_cast<std::size_t>(index)];
        if (event.descriptor == listening->get()) {
          if (event.readable &&
              !accept_clients(listening->get(), event_queue, clients)) {
            std::cerr << "accept failure: " << std::strerror(errno) << '\n';
            loop_failed = true;
            break;
          }
          continue;
        }
        const auto found = clients.find(event.descriptor);
        if (found == clients.end()) continue;
        Connection& connection = found->second;
        bool keep = true;
        if (event.readable) keep = read_client(connection, mutable_database);
        if (keep && !connection.input.empty())
          keep = process_input(connection, mutable_database);
        if (keep && event.writable) keep = write_client(connection);
        if (keep && connection.output.empty() && !connection.input.empty())
          keep = process_input(connection, mutable_database);
        if (keep && !connection.output.empty())
          keep = event_queue.set_writable(connection.descriptor, true);
        if (keep && connection.output.empty())
          keep = event_queue.set_writable(connection.descriptor, false);
        if (keep && connection.peer_closed && connection.output.empty())
          keep = false;
        if (event.error && !event.readable && !event.writable) keep = false;
        if (!keep) close_client(clients, event.descriptor);
      }
    }
    if (loop_failed) break;
    close_idle_clients(clients);
  }

  for (const auto& [descriptor, connection] : clients) {
    static_cast<void>(connection);
    close(descriptor);
  }
  if (const auto closed = mutable_database.close(); !closed) {
    std::cerr << "failed to close database\n";
    return 1;
  }
  return loop_failed ? 1 : 0;
}
