#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <tavern/database.hpp>
#include <utility>
#include <vector>

namespace {

class Descriptor {
 public:
  explicit Descriptor(int descriptor = -1) : descriptor_(descriptor) {}
  ~Descriptor() {
    if (descriptor_ >= 0) close(descriptor_);
  }
  Descriptor(const Descriptor&) = delete;
  auto operator=(const Descriptor&) -> Descriptor& = delete;
  Descriptor(Descriptor&& other) noexcept
      : descriptor_(std::exchange(other.descriptor_, -1)) {}
  auto get() const -> int { return descriptor_; }
  auto release() -> int { return std::exchange(descriptor_, -1); }

 private:
  int descriptor_;
};

auto database_path() -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         ("tavern-cli-" + std::to_string(static_cast<long long>(getpid())) +
          ".db");
}

class ServerProcess {
 public:
  ~ServerProcess() { stop(); }

  auto start(const std::filesystem::path& database)
      -> std::optional<std::uint16_t> {
    int pipe_descriptors[2];
    if (pipe(pipe_descriptors) != 0) return std::nullopt;
    child_ = fork();
    if (child_ == 0) {
      close(pipe_descriptors[0]);
      dup2(pipe_descriptors[1], STDOUT_FILENO);
      close(pipe_descriptors[1]);
      execl(TAVERN_SERVER, TAVERN_SERVER, "--database", database.c_str(),
            "--port", "0", static_cast<char*>(nullptr));
      _exit(127);
    }
    close(pipe_descriptors[1]);
    Descriptor output(pipe_descriptors[0]);
    fd_set ready;
    FD_ZERO(&ready);
    FD_SET(output.get(), &ready);
    timeval timeout{5, 0};
    if (child_ < 0 ||
        select(output.get() + 1, &ready, nullptr, nullptr, &timeout) != 1)
      return std::nullopt;
    std::string line;
    char character = 0;
    while (read(output.get(), &character, 1) == 1) {
      line.push_back(character);
      if (character == '\n') break;
    }
    constexpr std::string_view prefix = "listening on 127.0.0.1:";
    if (!line.starts_with(prefix)) return std::nullopt;
    unsigned long port = 0;
    for (const char character : line.substr(prefix.size())) {
      if (character == '\n') break;
      if (character < '0' || character > '9') return std::nullopt;
      port = port * 10 + static_cast<unsigned long>(character - '0');
    }
    return port > 0 && port <= 65535
               ? std::optional(static_cast<std::uint16_t>(port))
               : std::nullopt;
  }

  auto stop() -> bool {
    if (child_ <= 0) return true;
    const pid_t child = std::exchange(child_, -1);
    if (kill(child, SIGTERM) != 0 && errno != ESRCH) return false;
    int status = 0;
    return waitpid(child, &status, 0) == child && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
  }

 private:
  pid_t child_ = -1;
};

struct CliResult {
  int status;
  std::string output;
};

auto run_cli(const std::vector<std::string>& arguments,
             std::string_view input = {}) -> std::optional<CliResult> {
  int input_pipe[2];
  int output_pipe[2];
  if (pipe(input_pipe) != 0 || pipe(output_pipe) != 0) return std::nullopt;
  const pid_t child = fork();
  if (child == 0) {
    close(input_pipe[1]);
    close(output_pipe[0]);
    dup2(input_pipe[0], STDIN_FILENO);
    dup2(output_pipe[1], STDOUT_FILENO);
    close(input_pipe[0]);
    close(output_pipe[1]);
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 2);
    argv.push_back(const_cast<char*>(TAVERN_CLI));
    for (const std::string& argument : arguments)
      argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    execv(TAVERN_CLI, argv.data());
    _exit(127);
  }
  close(input_pipe[0]);
  close(output_pipe[1]);
  Descriptor writer(input_pipe[1]);
  Descriptor reader(output_pipe[0]);
  if (!input.empty() && write(writer.get(), input.data(), input.size()) !=
                            static_cast<ssize_t>(input.size()))
    return std::nullopt;
  close(writer.release());
  std::string output;
  char buffer[256];
  while (true) {
    const auto count = read(reader.get(), buffer, sizeof(buffer));
    if (count > 0)
      output.append(buffer, static_cast<std::size_t>(count));
    else if (count < 0 && errno == EINTR)
      continue;
    else
      break;
  }
  int status = 0;
  if (child <= 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status))
    return std::nullopt;
  return CliResult{.status = WEXITSTATUS(status), .output = std::move(output)};
}

auto options(std::uint16_t port) -> std::vector<std::string> {
  return {"--host", "127.0.0.1", "--port", std::to_string(port)};
}

auto append_and_expect(std::vector<std::string> arguments,
                       std::string_view command, int status,
                       std::string_view response) -> bool {
  arguments.emplace_back(command);
  const auto result = run_cli(arguments);
  return result && result->status == status && result->output == response;
}

auto fails(const std::vector<std::string>& arguments) -> bool {
  const auto result = run_cli(arguments);
  return result && result->status != 0;
}

}  // namespace

auto main() -> int {
  const auto path = database_path();
  std::filesystem::remove(path);
  auto database = tavern::Database::create(path);
  if (!database || !database->close()) return 1;

  ServerProcess server;
  const auto port = server.start(path);
  if (!port) return 1;
  const auto base = options(*port);
  if (!append_and_expect(base, "FOLLOW 1 2", 0, "OK\n") ||
      !append_and_expect(base, "IS_FOLLOWING 1 2", 0, "BOOL 1\n") ||
      !append_and_expect(base, "FOLLOWING 1", 0, "IDS 1 2\n") ||
      !append_and_expect(base, "NOT_A_COMMAND", 1, "ERR UNKNOWN_COMMAND\n"))
    return 1;

  const auto interactive =
      run_cli(base, "BEFRIEND 1 3\nARE_FRIENDS 1 3\nEXIT\nFOLLOW 9 10\n");
  if (!interactive || interactive->status != 0 ||
      interactive->output != "OK\nBOOL 1\n")
    return 1;
  const auto eof = run_cli(base, "FOLLOWERS 2\n");
  if (!eof || eof->status != 0 || eof->output != "IDS 1 1\n") return 1;

  if (!fails({"--port", "0", "FOLLOW", "1", "2"}) ||
      !fails({"--host", "not-an-ip", "FOLLOW", "1", "2"}) ||
      !fails({"--port", "1", "FOLLOW", "1", "2"}) ||
      !fails({std::string(4097, 'X')}))
    return 1;

  if (!server.stop()) return 1;
  const auto restarted_port = server.start(path);
  if (!restarted_port ||
      !append_and_expect(options(*restarted_port), "ARE_FRIENDS 3 1", 0,
                         "BOOL 1\n") ||
      !server.stop())
    return 1;
  std::filesystem::remove(path);
  return 0;
}
