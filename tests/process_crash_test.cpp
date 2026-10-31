#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "database_internal.hpp"
#include "reachability.hpp"

namespace {

auto point(int value) -> tavern::CrashPoint {
  return static_cast<tavern::CrashPoint>(value);
}

auto child(const char* file, int raw, bool friend_op) -> int {
  auto database = tavern::open_database(file);
  if (!database) return 2;
  tavern::set_crash_point_for_test(point(raw));
  const auto result = friend_op ? tavern::befriend(*database, 3, 4)
                                : tavern::follow(*database, 3, 4);
  return result ? 3 : 4;
}

auto run(const char* self, const std::filesystem::path& file, int raw,
         bool friend_op) -> bool {
  auto database = tavern::create_database(file);
  if (!database || !tavern::follow(*database, 1, 2) ||
      !tavern::close_database(*database))
    return false;
  const pid_t pid = fork();
  if (pid == 0) {
    execl(self, self, "child", file.c_str(), std::to_string(raw).c_str(),
          friend_op ? "friend" : "follow", static_cast<char*>(nullptr));
    _exit(127);
  }
  int status = 0;
  if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
      WEXITSTATUS(status) != 99)
    return false;
  auto reopened = tavern::open_database(file);
  if (!reopened || !tavern::is_following(*reopened, 1, 2).value()) return false;
  bool reverse = false;
  const auto first = friend_op ? tavern::are_friends(*reopened, 3, 4)
                               : tavern::is_following(*reopened, 3, 4);
  if (!first) return false;
  if (friend_op) {
    const auto second = tavern::are_friends(*reopened, 4, 3);
    if (!second || *first != *second) return false;
    reverse = *second;
  } else {
    const auto second = tavern::followers(
        *reopened, 4,
        [&](tavern::PersonId id) -> std::expected<void, tavern::Error> {
          reverse = reverse || id == 3;
          return std::expected<void, tavern::Error>{};
        });
    if (!second || *first != reverse) return false;
  }
  const bool present = *first;
  const bool absent =
      raw == static_cast<int>(tavern::CrashPoint::before_data_sync) ||
      raw == static_cast<int>(tavern::CrashPoint::after_data_sync);
  const bool durable =
      raw == static_cast<int>(tavern::CrashPoint::after_metadata_sync);
  if ((absent && present) || (durable && !present) ||
      database_impl(*reopened).commit.generation != (present ? 2U : 1U))
    return false;
  if (!tavern::befriend(*reopened, 5, 6) || !tavern::close_database(*reopened))
    return false;
  auto final = tavern::open_database(file);
  const bool ok = final && tavern::is_following(*final, 1, 2).value() &&
                  tavern::are_friends(*final, 5, 6).value();
  if (final) (void)tavern::close_database(*final);
  return ok;
}

auto candidates(tavern::Database& database) -> bool {
  const std::array<std::byte, 1> payload{std::byte{0x7A}};
  for (int count = 0; count < 4; ++count)
    if (!tavern::append_page(database_impl(database).store,
                             tavern::PageType::data, payload))
      return false;
  const auto scan = tavern::scan_reachable_pages(database_impl(database).store);
  return scan && scan->unreachable_data_pages >= 4;
}

auto reuse_child(const char* file, int raw) -> int {
  auto database = tavern::open_database(file);
  if (!database || !tavern::initialize_page_liveness_shadow_for_test(*database))
    return 2;
  if (raw == static_cast<int>(tavern::CrashPoint::during_reusable_write))
    database_impl(*database).store.fail_reusable_write_after_for_test = 1;
  tavern::set_crash_point_for_test(point(raw));
  const auto result = tavern::follow_with_index_reuse_for_test(*database, 4, 5);
  return result ? 3 : 4;
}

auto run_reuse(const char* self, const std::filesystem::path& file, int raw)
    -> bool {
  auto database = tavern::create_database(file);
  if (!database || !tavern::follow(*database, 1, 2) ||
      !tavern::befriend(*database, 1, 3) || !candidates(*database))
    return false;
  const tavern::PageId physical_pages =
      database_impl(*database).store.page_count;
  const auto scan =
      tavern::scan_reachable_pages(database_impl(*database).store);
  tavern::PageId first_reusable = tavern::invalid_page_id;
  if (scan)
    for (tavern::PageId id = tavern::first_data_page;
         id < database_impl(*database).store.page_count; ++id)
      if (!scan->reachable[static_cast<std::size_t>(id)]) {
        first_reusable = id;
        break;
      }
  if (!scan || scan->unreachable_data_pages < 4 ||
      first_reusable == tavern::invalid_page_id ||
      scan->reachable[static_cast<std::size_t>(first_reusable)] ||
      !tavern::close_database(*database))
    return false;
  const pid_t pid = fork();
  if (pid == 0) {
    execl(self, self, "reuse", file.c_str(), std::to_string(raw).c_str(),
          static_cast<char*>(nullptr));
    _exit(127);
  }
  int status = 0;
  if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
      WEXITSTATUS(status) != 99)
    return false;
  auto reopened = tavern::open_database(file);
  if (!reopened ||
      database_impl(*reopened).store.page_count != physical_pages ||
      !tavern::is_following(*reopened, 1, 2).value_or(false) ||
      !tavern::are_friends(*reopened, 1, 3).value_or(false) ||
      !tavern::are_friends(*reopened, 3, 1).value_or(false))
    return false;
  const auto new_follow = tavern::is_following(*reopened, 4, 5);
  if (!new_follow) return false;
  const bool durable =
      raw == static_cast<int>(tavern::CrashPoint::after_metadata_sync);
  if (*new_follow != durable ||
      database_impl(*reopened).commit.generation != (durable ? 3U : 2U))
    return false;
  const auto protected_scan =
      tavern::scan_protected_directories(database_impl(*reopened).store);
  const auto reachability =
      tavern::scan_reachable_pages(database_impl(*reopened).store);
  const bool valid_roots =
      protected_scan && protected_scan->roots.size() == 2 &&
      reachability.has_value() &&
      tavern::initialize_page_liveness_shadow_for_test(*reopened);
  const auto closed = tavern::close_database(*reopened);
  return valid_roots && closed.has_value();
}

}  // namespace

auto main(int argc, char** argv) -> int {
  if (argc == 5 && std::string(argv[1]) == "child")
    return child(argv[2], std::atoi(argv[3]), std::string(argv[4]) == "friend");
  if (argc == 4 && std::string(argv[1]) == "reuse")
    return reuse_child(argv[2], std::atoi(argv[3]));
  bool ok = true;
  for (int raw = 1; raw <= 4; ++raw)
    for (const bool friend_op : {false, true}) {
      const auto file =
          std::filesystem::temp_directory_path() /
          ("tavern-crash-" + std::to_string(static_cast<long long>(getpid())) +
           "-" + std::to_string(raw) + (friend_op ? "f" : "d") + ".db");
      std::filesystem::remove(file);
      ok = ok && run(argv[0], file, raw, friend_op);
      std::filesystem::remove(file);
    }
  for (const int raw :
       {static_cast<int>(tavern::CrashPoint::during_reusable_write),
        static_cast<int>(tavern::CrashPoint::after_data_sync),
        static_cast<int>(tavern::CrashPoint::after_metadata_sync)}) {
    const auto file = std::filesystem::temp_directory_path() /
                      ("tavern-reuse-crash-" +
                       std::to_string(static_cast<long long>(getpid())) + "-" +
                       std::to_string(raw) + ".db");
    std::filesystem::remove(file);
    ok = ok && run_reuse(argv[0], file, raw);
    std::filesystem::remove(file);
  }
  return ok ? 0 : 1;
}
