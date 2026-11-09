#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

// The client implements the wire framing directly rather than linking Tavern:
// it can be distributed independently and has no database-file access.
constexpr std::size_t maximum_request_size = 4096;
constexpr std::size_t maximum_response_size = 65536;

class FileDescriptor {
 public:
  explicit FileDescriptor(int descriptor = -1) : descriptor_(descriptor) {}
  ~FileDescriptor() {
    if (descriptor_ >= 0) close(descriptor_);
  }
  FileDescriptor(const FileDescriptor&) = delete;
  auto operator=(const FileDescriptor&) -> FileDescriptor& = delete;
  FileDescriptor(FileDescriptor&& other) noexcept
      : descriptor_(std::exchange(other.descriptor_, -1)) {}
  auto get() const -> int { return descriptor_; }

 private:
  int descriptor_;
};

struct Options {
  std::string host = "127.0.0.1";
  std::uint16_t port = 7373;
  int command_index = 0;
};

void usage() {
  std::cerr << "usage: tavern-cli [--host <ipv4>] [--port <port>] "
               "[COMMAND [ARGS...]]\n";
}

auto parse_port(std::string_view value) -> std::optional<std::uint16_t> {
  if (value.empty()) return std::nullopt;
  unsigned long parsed = 0;
  for (const char character : value) {
    if (character < '0' || character > '9') return std::nullopt;
    const auto digit = static_cast<unsigned long>(character - '0');
    if (parsed > (std::numeric_limits<std::uint16_t>::max() - digit) / 10)
      return std::nullopt;
    parsed = parsed * 10 + digit;
  }
  if (parsed == 0) return std::nullopt;
  return static_cast<std::uint16_t>(parsed);
}

auto parse_options(int argc, char** argv) -> std::optional<Options> {
  Options options;
  int index = 1;
  while (index < argc && std::string_view(argv[index]).starts_with("--")) {
    const std::string_view option(argv[index]);
    if ((option != "--host" && option != "--port") || index + 1 >= argc) {
      usage();
      return std::nullopt;
    }
    const std::string_view value(argv[index + 1]);
    if (option == "--host") {
      options.host = value;
    } else {
      const auto port = parse_port(value);
      if (!port) {
        std::cerr << "invalid port\n";
        return std::nullopt;
      }
      options.port = *port;
    }
    index += 2;
  }
  options.command_index = index;
  return options;
}

auto connect_to(const Options& options) -> std::optional<FileDescriptor> {
  sockaddr_in endpoint{};
  endpoint.sin_family = AF_INET;
  endpoint.sin_port = htons(options.port);
  if (options.host.find(':') != std::string::npos ||
      inet_pton(AF_INET, options.host.c_str(), &endpoint.sin_addr) != 1) {
    std::cerr << "host must be an IPv4 address\n";
    return std::nullopt;
  }
  FileDescriptor descriptor(socket(AF_INET, SOCK_STREAM, 0));
  if (descriptor.get() < 0 ||
      connect(descriptor.get(), reinterpret_cast<const sockaddr*>(&endpoint),
              sizeof(endpoint)) != 0) {
    std::cerr << "connection failed: " << std::strerror(errno) << '\n';
    return std::nullopt;
  }
  return descriptor;
}

auto send_all(int descriptor, std::string_view request) -> bool {
  // Blocking TCP writes can be partial or interrupted. SIGPIPE is ignored in
  // main, turning a peer-close during send into the diagnostic path below.
  while (!request.empty()) {
    const auto written = send(descriptor, request.data(), request.size(), 0);
    if (written > 0) {
      request.remove_prefix(static_cast<std::size_t>(written));
    } else if (written < 0 && errno == EINTR) {
      continue;
    } else {
      std::cerr << "send failed: " << std::strerror(errno) << '\n';
      return false;
    }
  }
  return true;
}

auto receive_response(int descriptor) -> std::optional<std::string> {
  // Requests are strictly sequential, so the client expects one response before
  // issuing another. Accumulate arbitrary recv fragments through the first LF;
  // recv boundaries have no protocol meaning.
  std::string response;
  response.reserve(128);
  char buffer[1024];
  while (true) {
    const auto received = recv(descriptor, buffer, sizeof(buffer), 0);
    if (received > 0) {
      const auto count = static_cast<std::size_t>(received);
      for (std::size_t index = 0; index < count; ++index) {
        response.push_back(buffer[index]);
        if (response.size() > maximum_response_size) {
          std::cerr << "response exceeds protocol limit\n";
          return std::nullopt;
        }
        if (buffer[index] == '\n') return response;
      }
    } else if (received == 0) {
      std::cerr << "connection closed before response\n";
      return std::nullopt;
    } else if (errno != EINTR) {
      std::cerr << "receive failed: " << std::strerror(errno) << '\n';
      return std::nullopt;
    }
  }
}

auto execute_request(int descriptor, std::string_view request,
                     std::string& response) -> bool {
  if (request.size() > maximum_request_size) {
    std::cerr << "request exceeds protocol limit\n";
    return false;
  }
  // The CLI adds the framing delimiter; command arguments themselves are sent
  // unchanged after one-shot joining or one interactive input line.
  std::string line(request);
  line.push_back('\n');
  if (!send_all(descriptor, line)) return false;
  const auto received = receive_response(descriptor);
  if (!received) return false;
  response = *received;
  return true;
}

auto join_command(int argc, char** argv, int index)
    -> std::optional<std::string> {
  std::string request;
  for (; index < argc; ++index) {
    if (!request.empty()) request.push_back(' ');
    request += argv[index];
    if (request.size() > maximum_request_size) {
      std::cerr << "request exceeds protocol limit\n";
      return std::nullopt;
    }
  }
  return request;
}

auto interactive(FileDescriptor& connection) -> int {
  // Interactive mode keeps one connection for sequential requests. EXIT and
  // QUIT are intentionally client-local, so they never depend on server
  // support.
  std::string request;
  while (std::getline(std::cin, request)) {
    if (request == "QUIT" || request == "EXIT") return 0;
    std::string response;
    if (!execute_request(connection.get(), request, response)) return 1;
    std::cout << response << std::flush;
  }
  return std::cin.eof() ? 0 : 1;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  std::signal(SIGPIPE, SIG_IGN);
  const auto options = parse_options(argc, argv);
  if (!options) return 1;
  auto connection = connect_to(*options);
  if (!connection) return 1;
  if (options->command_index == argc) return interactive(*connection);
  const auto request = join_command(argc, argv, options->command_index);
  if (!request) return 1;
  std::string response;
  if (!execute_request(connection->get(), *request, response)) return 1;
  // In one-shot mode stdout remains exactly the server frame for scripts;
  // diagnostics use stderr and an ERR or transport failure has nonzero status.
  std::cout << response << std::flush;
  return response.starts_with("ERR ") ? 1 : 0;
}
