#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <tavern/database.hpp>
#include <utility>

namespace {

class Descriptor {
 public:
  explicit Descriptor(int descriptor = -1) : descriptor_(descriptor) {}
  ~Descriptor() {
    if (descriptor_ >= 0) close(descriptor_);
  }
  Descriptor(const Descriptor&) = delete;
  auto operator=(const Descriptor&) -> Descriptor& = delete;
  Descriptor(Descriptor&& other) noexcept : descriptor_(other.release()) {}
  auto operator=(Descriptor&& other) noexcept -> Descriptor& {
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

auto database_path() -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         ("tavern-tcp-" + std::to_string(static_cast<long long>(getpid())) +
          ".db");
}

auto send_all(int descriptor, std::string_view message) -> bool {
  while (!message.empty()) {
    const auto written = send(descriptor, message.data(), message.size(), 0);
    if (written > 0) {
      message.remove_prefix(static_cast<std::size_t>(written));
    } else if (written < 0 && errno == EINTR) {
      continue;
    } else {
      return false;
    }
  }
  return true;
}

auto receive_line(int descriptor) -> std::optional<std::string> {
  std::string line;
  char character = 0;
  while (true) {
    const auto received = recv(descriptor, &character, 1, 0);
    if (received == 1) {
      line.push_back(character);
      if (character == '\n') return line;
    } else if (received < 0 && errno == EINTR) {
      continue;
    } else {
      return std::nullopt;
    }
  }
}

auto receive_line_with_timeout(int descriptor, std::chrono::seconds timeout)
    -> std::optional<std::string> {
  std::string line;
  char character = 0;
  while (true) {
    fd_set ready;
    FD_ZERO(&ready);
    FD_SET(descriptor, &ready);
    timeval wait{
        .tv_sec = static_cast<time_t>(timeout.count()),
        .tv_usec = 0,
    };
    const int selected =
        select(descriptor + 1, &ready, nullptr, nullptr, &wait);
    if (selected == 0) return std::nullopt;
    if (selected < 0 && errno == EINTR) continue;
    if (selected < 0) return std::nullopt;
    const auto received = recv(descriptor, &character, 1, 0);
    if (received == 1) {
      line.push_back(character);
      if (character == '\n') return line;
    } else if (received < 0 && errno == EINTR) {
      continue;
    } else {
      return std::nullopt;
    }
  }
}

class ServerProcess {
 public:
  ServerProcess() = default;
  ~ServerProcess() { stop(); }
  ServerProcess(const ServerProcess&) = delete;
  auto operator=(const ServerProcess&) -> ServerProcess& = delete;

  auto start(const std::filesystem::path& path,
             std::string_view host = "127.0.0.1")
      -> std::optional<std::uint16_t> {
    if (!stop()) return std::nullopt;
    const std::string host_argument(host);
    int pipe_descriptors[2];
    if (pipe(pipe_descriptors) != 0) return std::nullopt;
    child_ = fork();
    if (child_ == 0) {
      close(pipe_descriptors[0]);
      dup2(pipe_descriptors[1], STDOUT_FILENO);
      close(pipe_descriptors[1]);
      execl(TAVERN_SERVER, TAVERN_SERVER, "--database", path.c_str(), "--host",
            host_argument.c_str(), "--port", "0", static_cast<char*>(nullptr));
      _exit(127);
    }
    close(pipe_descriptors[1]);
    Descriptor output(pipe_descriptors[0]);
    fd_set ready;
    FD_ZERO(&ready);
    FD_SET(output.get(), &ready);
    timeval timeout{5, 0};
    if (child_ < 0 ||
        select(output.get() + 1, &ready, nullptr, nullptr, &timeout) != 1) {
      stop();
      return std::nullopt;
    }
    std::string startup;
    char character = 0;
    while (read(output.get(), &character, 1) == 1) {
      startup.push_back(character);
      if (character == '\n') break;
    }
    const std::string prefix = "listening on " + std::string(host) + ':';
    if (!startup.starts_with(prefix) || startup.back() != '\n') {
      stop();
      return std::nullopt;
    }
    const std::string port =
        startup.substr(prefix.size(), startup.size() - prefix.size() - 1);
    unsigned long value = 0;
    for (const char character : port) {
      if (character < '0' || character > '9' || value > 6553) {
        stop();
        return std::nullopt;
      }
      value = value * 10 + static_cast<unsigned long>(character - '0');
    }
    if (value == 0 || value > 65535) {
      stop();
      return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
  }

  auto stop() -> bool {
    if (child_ <= 0) return true;
    const pid_t child = std::exchange(child_, -1);
    if (kill(child, SIGTERM) != 0 && errno != ESRCH) return false;
    int status = 0;
    pid_t waited = 0;
    do {
      waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    return waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  }

 private:
  pid_t child_ = -1;
};

auto connect_to(std::uint16_t port) -> Descriptor {
  Descriptor socket_descriptor(socket(AF_INET, SOCK_STREAM, 0));
  if (socket_descriptor.get() < 0) return socket_descriptor;
  sockaddr_in endpoint{};
  endpoint.sin_family = AF_INET;
  endpoint.sin_port = htons(port);
  endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(socket_descriptor.get(),
              reinterpret_cast<const sockaddr*>(&endpoint),
              sizeof(endpoint)) != 0)
    return Descriptor{};
  return socket_descriptor;
}

auto request(int descriptor, std::string_view command,
             std::string_view expected) -> bool {
  if (!send_all(descriptor, command)) return false;
  const auto response = receive_line(descriptor);
  return response && *response == expected;
}

auto request_with_timeout(int descriptor, std::string_view command,
                          std::string_view expected,
                          std::chrono::seconds timeout) -> bool {
  if (!send_all(descriptor, command)) return false;
  const auto response = receive_line_with_timeout(descriptor, timeout);
  return response && *response == expected;
}

auto expect_start_failure(const std::filesystem::path& path,
                          std::string_view diagnostic = {}) -> bool {
  int pipe_descriptors[2];
  if (pipe(pipe_descriptors) != 0) return false;
  const pid_t child = fork();
  if (child == 0) {
    close(pipe_descriptors[0]);
    dup2(pipe_descriptors[1], STDERR_FILENO);
    close(pipe_descriptors[1]);
    execl(TAVERN_SERVER, TAVERN_SERVER, "--database", path.c_str(), "--port",
          "0", static_cast<char*>(nullptr));
    _exit(127);
  }
  close(pipe_descriptors[1]);
  Descriptor output(pipe_descriptors[0]);
  int status = 0;
  if (child <= 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
      WEXITSTATUS(status) == 0)
    return false;
  std::string message;
  char buffer[128];
  while (true) {
    const auto read_count = read(output.get(), buffer, sizeof(buffer));
    if (read_count > 0) {
      message.append(buffer, static_cast<std::size_t>(read_count));
    } else if (read_count < 0 && errno == EINTR) {
      continue;
    } else {
      break;
    }
  }
  return diagnostic.empty() || message.find(diagnostic) != std::string::npos;
}

auto expect_host_failure(const std::filesystem::path& path,
                         std::string_view host, std::string_view diagnostic)
    -> bool {
  int pipe_descriptors[2];
  if (pipe(pipe_descriptors) != 0) return false;
  const std::string host_argument(host);
  const pid_t child = fork();
  if (child == 0) {
    close(pipe_descriptors[0]);
    dup2(pipe_descriptors[1], STDERR_FILENO);
    close(pipe_descriptors[1]);
    execl(TAVERN_SERVER, TAVERN_SERVER, "--database", path.c_str(), "--host",
          host_argument.c_str(), "--port", "0", static_cast<char*>(nullptr));
    _exit(127);
  }
  close(pipe_descriptors[1]);
  Descriptor output(pipe_descriptors[0]);
  int status = 0;
  if (child <= 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
      WEXITSTATUS(status) == 0)
    return false;
  std::string message;
  char buffer[128];
  while (true) {
    const auto read_count = read(output.get(), buffer, sizeof(buffer));
    if (read_count > 0) {
      message.append(buffer, static_cast<std::size_t>(read_count));
    } else if (read_count < 0 && errno == EINTR) {
      continue;
    } else {
      break;
    }
  }
  return message.find(diagnostic) != std::string::npos;
}

auto test_server(bool full_response_boundary) -> bool {
  const auto path = database_path();
  const auto missing_path = path.string() + ".missing";
  const auto invalid_path = path.string() + ".invalid";
  std::filesystem::remove(path);
  std::filesystem::remove(missing_path);
  std::filesystem::remove(invalid_path);
  auto database = tavern::Database::create(path);
  if (!database || !database->close() ||
      !expect_start_failure(missing_path, "database file does not exist"))
    return false;
  {
    std::ofstream invalid(invalid_path);
    invalid << "not a Tavern database";
  }
  if (!expect_start_failure(invalid_path, "corrupt or uses an unsupported"))
    return false;
  auto locked = tavern::Database::open(path);
  if (!locked || !expect_start_failure(path, "database is locked"))
    return false;
  if (!locked->close()) return false;

  if (!expect_host_failure(path, "not-an-ip", "invalid IPv4 host") ||
      !expect_host_failure(path, "::1", "IPv6 hosts are unsupported"))
    return false;

  ServerProcess wildcard_server;
  const auto wildcard_port = wildcard_server.start(path, "0.0.0.0");
  Descriptor wildcard_client =
      wildcard_port ? connect_to(*wildcard_port) : Descriptor{};
  if (!wildcard_port || wildcard_client.get() < 0 ||
      !request(wildcard_client.get(), "FOLLOW 6 7\n", "OK\n") ||
      !wildcard_server.stop())
    return false;

  ServerProcess server;
  const auto port = server.start(path, "127.0.0.1");
  if (!port) return false;

  // A second server cannot own the database while the first one is running.
  if (!expect_start_failure(path, "database is locked")) return false;

  // A fragmented client must not delay requests from another connection.
  Descriptor fragmented_client = connect_to(*port);
  Descriptor active_client = connect_to(*port);
  if (fragmented_client.get() < 0 || active_client.get() < 0 ||
      !send_all(fragmented_client.get(), "FOLLOW 40 ") ||
      !request_with_timeout(active_client.get(), "FOLLOW 8 9\n", "OK\n",
                            std::chrono::seconds(5)) ||
      !send_all(fragmented_client.get(), "41\n"))
    return false;
  const auto fragmented_mutation = receive_line(fragmented_client.get());
  if (!fragmented_mutation || *fragmented_mutation != "OK\n" ||
      !request(fragmented_client.get(), "IS_FOLLOWING 40 41\n", "BOOL 1\n"))
    return false;

  // Requests sent on separate persistent connections are independently framed.
  Descriptor interleaved_a = connect_to(*port);
  Descriptor interleaved_b = connect_to(*port);
  if (interleaved_a.get() < 0 || interleaved_b.get() < 0 ||
      !send_all(interleaved_a.get(), "BEFRIEND 50 51\n") ||
      !send_all(interleaved_b.get(), "FOLLOW 52 53\n"))
    return false;
  const auto interleaved_a_mutation = receive_line(interleaved_a.get());
  const auto interleaved_b_mutation = receive_line(interleaved_b.get());
  if (!interleaved_a_mutation || !interleaved_b_mutation ||
      *interleaved_a_mutation != "OK\n" || *interleaved_b_mutation != "OK\n" ||
      !request(interleaved_b.get(), "IS_FOLLOWING 52 53\n", "BOOL 1\n") ||
      !request(interleaved_a.get(), "ARE_FRIENDS 50 51\n", "BOOL 1\n"))
    return false;

  // Client A creates the durable graph, using fragmented CRLF input.
  Descriptor client_a = connect_to(*port);
  if (client_a.get() < 0 || !send_all(client_a.get(), "FOLLOW 1 ") ||
      !send_all(client_a.get(), "2\r\n"))
    return false;
  const auto fragmented = receive_line(client_a.get());
  if (!fragmented || *fragmented != "OK\n" ||
      !request(client_a.get(), "BEFRIEND 1 3\n", "OK\n") ||
      !request(client_a.get(), "BEFRIEND 2 3\n", "OK\n") ||
      !request(client_a.get(), "IS_FOLLOWING 1 2\n", "BOOL 1\n") ||
      !request(client_a.get(), "ARE_FRIENDS 3 1\n", "BOOL 1\n"))
    return false;
  client_a = Descriptor{};

  // An incomplete request at EOF must not prevent the next sequential client.
  {
    Descriptor disconnected = connect_to(*port);
    if (disconnected.get() < 0 || !send_all(disconnected.get(), "FOLLOW 99"))
      return false;
  }

  // Client B observes A's graph and sends several requests on one connection.
  Descriptor commands = connect_to(*port);
  if (commands.get() < 0 ||
      !send_all(commands.get(),
                "IS_FOLLOWING 1 2\nIS_FOLLOWING 2 1\nIS_FOLLOWING 99 0\n"
                "ARE_FRIENDS 3 1\nFOLLOWING 1\nFOLLOWERS 2\nFRIENDS 1\n"
                "MUTUAL_FRIENDS 1 2\nFOLLOW 4 5\nUNFOLLOW 4 5\n"
                "BEFRIEND 4 6\nUNFRIEND 4 6\n"))
    return false;
  const std::string_view expected[] = {"BOOL 1\n",  "BOOL 0\n",  "BOOL 0\n",
                                       "BOOL 1\n",  "IDS 1 2\n", "IDS 1 1\n",
                                       "IDS 1 3\n", "IDS 1 3\n", "OK\n",
                                       "OK\n",      "OK\n",      "OK\n"};
  for (const auto response : expected) {
    const auto received = receive_line(commands.get());
    if (!received || *received != response) return false;
  }

  // Bad input has exactly one error response and does not mutate the graph.
  if (!send_all(commands.get(),
                "NOT_A_COMMAND\nFOLLOW 1 bad\nIS_FOLLOWING 1 2\n"))
    return false;
  const auto invalid = receive_line(commands.get());
  const auto malformed = receive_line(commands.get());
  const auto unchanged = receive_line(commands.get());
  if (!invalid || !malformed || !unchanged ||
      *invalid != "ERR UNKNOWN_COMMAND\n" ||
      *malformed != "ERR BAD_REQUEST\n" || *unchanged != "BOOL 1\n")
    return false;
  if (!send_all(commands.get(), std::string(4097, 'X') + "\nFOLLOW 6 7\n"))
    return false;
  const auto oversized = receive_line(commands.get());
  const auto resynchronized = receive_line(commands.get());
  if (!oversized || !resynchronized ||
      *oversized != "ERR REQUEST_TOO_LARGE\n" || *resynchronized != "OK\n")
    return false;

  if (full_response_boundary) {
    // Exercise the exact 65536-byte response boundary through TCP, then force
    // an oversized list and confirm that no partial list is sent.
    std::string boundary_mutations;
    boundary_mutations.reserve(100000);
    boundary_mutations += "FOLLOW 100 999999\n";
    const auto maximum = std::numeric_limits<tavern::PersonId>::max();
    for (tavern::PersonId index = 0; index < 3120; ++index) {
      boundary_mutations += "FOLLOW 100 ";
      boundary_mutations += std::to_string(maximum - index);
      boundary_mutations += '\n';
    }
    if (!send_all(commands.get(), boundary_mutations)) return false;
    for (std::size_t index = 0; index < 3121; ++index) {
      const auto response = receive_line(commands.get());
      if (!response || *response != "OK\n") return false;
    }
    if (!send_all(commands.get(), "FOLLOWING 100\n")) return false;
    Descriptor active_during_large_response = connect_to(*port);
    if (active_during_large_response.get() < 0 ||
        !request(active_during_large_response.get(), "IS_FOLLOWING 1 2\n",
                 "BOOL 1\n"))
      return false;
    {
      Descriptor disconnected_during_output = connect_to(*port);
      if (disconnected_during_output.get() < 0 ||
          !send_all(disconnected_during_output.get(), "FOLLOWING 100\n"))
        return false;
    }
    const auto exact_boundary = receive_line(commands.get());
    if (!exact_boundary || exact_boundary->size() != 65536 ||
        !exact_boundary->starts_with("IDS 3121 ") ||
        exact_boundary->back() != '\n')
      return false;
    const std::string extra =
        "FOLLOW 100 " + std::to_string(maximum - 3120) + "\n";
    if (!request(commands.get(), extra, "OK\n") ||
        !request(commands.get(), "FOLLOWING 100\n", "ERR RESPONSE_TOO_LARGE\n"))
      return false;
  }

  commands = Descriptor{};
  if (!server.stop()) return false;

  // Restart against the same file and verify durable directionality/symmetry.
  const auto restarted_port = server.start(path);
  if (!restarted_port) return false;
  Descriptor restarted = connect_to(*restarted_port);
  if (restarted.get() < 0 ||
      !request(restarted.get(), "IS_FOLLOWING 1 2\n", "BOOL 1\n") ||
      !request(restarted.get(), "IS_FOLLOWING 2 1\n", "BOOL 0\n") ||
      !request(restarted.get(), "ARE_FRIENDS 1 3\n", "BOOL 1\n") ||
      !request(restarted.get(), "ARE_FRIENDS 3 1\n", "BOOL 1\n") ||
      !request(restarted.get(), "MUTUAL_FRIENDS 1 2\n", "IDS 1 3\n"))
    return false;
  restarted = Descriptor{};
  if (!server.stop()) return false;

  // Server shutdown releases ownership for the installed public API.
  auto reopened = tavern::Database::open(path);
  if (!reopened || !reopened->is_following(1, 2).value_or(false) ||
      reopened->is_following(2, 1).value_or(true) ||
      !reopened->are_friends(3, 1).value_or(false) || !reopened->close())
    return false;
  std::filesystem::remove(path);
  std::filesystem::remove(invalid_path);
  return true;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const bool full_response_boundary =
      argc == 2 && std::string_view(argv[1]) == "--slow-response-boundary";
  return (argc == 1 || full_response_boundary) &&
                 test_server(full_response_boundary)
             ? 0
             : 1;
}
