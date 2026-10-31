#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>

#include "database.hpp"

namespace {

auto path(const char* suffix) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         (std::string("tavern-database-lock-") +
          std::to_string(static_cast<long long>(getpid())) + suffix);
}

auto locked(const std::expected<tavern::Database, tavern::Error>& result)
    -> bool {
  return !result && result.error() == tavern::Error::database_locked;
}

auto child_open(const char* file) -> int {
  return locked(tavern::open_database(file)) ? 0 : 1;
}

auto child_owner(const char* file, int ready) -> int {
  auto database = tavern::open_database(file);
  if (!database || write(ready, "x", 1) != 1) return 1;
  pause();
  return 1;
}

auto same_process_and_close() -> bool {
  const auto file = path("-same.db");
  std::filesystem::remove(file);
  auto owner = tavern::create_database(file);
  if (!owner || !tavern::follow(*owner, 1, 2) ||
      !locked(tavern::open_database(file)) ||
      !tavern::is_following(*owner, 1, 2).value_or(false) ||
      !tavern::close_database(*owner))
    return false;
  auto reopened = tavern::open_database(file);
  const bool result = reopened &&
                      tavern::is_following(*reopened, 1, 2).value_or(false) &&
                      tavern::close_database(*reopened);
  std::filesystem::remove(file);
  return result;
}

auto process_ownership(const char* self) -> bool {
  const auto file = path("-process.db");
  std::filesystem::remove(file);
  auto owner = tavern::create_database(file);
  if (!owner) return false;
  const pid_t contender = fork();
  if (contender == 0) {
    execl(self, self, "open", file.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  int status = 0;
  if (contender < 0 || waitpid(contender, &status, 0) != contender ||
      !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
      !tavern::close_database(*owner))
    return false;

  int ready[2]{};
  if (pipe(ready) != 0) return false;
  const pid_t child = fork();
  if (child == 0) {
    (void)close(ready[0]);
    execl(self, self, "owner", file.c_str(), std::to_string(ready[1]).c_str(),
          static_cast<char*>(nullptr));
    _exit(127);
  }
  (void)close(ready[1]);
  char signal = 0;
  const bool held = child > 0 && read(ready[0], &signal, 1) == 1 &&
                    locked(tavern::open_database(file));
  (void)close(ready[0]);
  if (child <= 0 || kill(child, SIGTERM) != 0 ||
      waitpid(child, &status, 0) != child || !WIFSIGNALED(status))
    return false;
  auto reopened = tavern::open_database(file);
  const bool result = held && reopened && tavern::close_database(*reopened);
  std::filesystem::remove(file);
  return result;
}

auto moves_preserve_ownership() -> bool {
  const auto first = path("-move-a.db");
  const auto second = path("-move-b.db");
  std::filesystem::remove(first);
  std::filesystem::remove(second);
  auto source = tavern::create_database(first);
  auto target = tavern::create_database(second);
  if (!source || !target) return false;
  tavern::Database moved(std::move(*source));
  if (!locked(tavern::open_database(first))) return false;
  *target = std::move(moved);
  if (!locked(tavern::open_database(first))) return false;
  auto released_second = tavern::open_database(second);
  if (!released_second || !tavern::close_database(*released_second) ||
      !tavern::close_database(*target))
    return false;
  auto released_first = tavern::open_database(first);
  const bool result = released_first && tavern::close_database(*released_first);
  std::filesystem::remove(first);
  std::filesystem::remove(second);
  return result;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  if (argc == 3 && std::string(argv[1]) == "open") return child_open(argv[2]);
  if (argc == 4 && std::string(argv[1]) == "owner")
    return child_owner(argv[2], std::stoi(argv[3]));
  if (!same_process_and_close()) {
    std::cerr << "same-process lock test failed\n";
    return 1;
  }
  if (!process_ownership(argv[0])) {
    std::cerr << "process lock test failed\n";
    return 1;
  }
  if (!moves_preserve_ownership()) {
    std::cerr << "move lock test failed\n";
    return 1;
  }
  return 0;
}
