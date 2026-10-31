#include <unistd.h>

#include <filesystem>
#include <string>

#include "database_internal.hpp"
namespace {
struct File {
  std::filesystem::path path;
  ~File() { std::filesystem::remove(path); }
};
auto path(const char* s) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         (std::string("tavern-failure-") +
          std::to_string(static_cast<long long>(getpid())) + s);
}
auto initial(tavern::Database& d) -> bool {
  return tavern::follow(d, 1, 2).has_value() &&
         tavern::is_following(d, 1, 2).value();
}
auto consistent_after(const std::filesystem::path& p) -> bool {
  auto d = tavern::open_database(p);
  if (!d) return false;
  const bool old = tavern::is_following(*d, 1, 2).value();
  const auto f = tavern::is_following(*d, 3, 4);
  const auto r = tavern::followers(
      *d, 4, [](tavern::PersonId) -> std::expected<void, tavern::Error> {
        return std::expected<void, tavern::Error>{};
      });
  const bool ok = old && f && r;
  tavern::close_database(*d);
  return ok;
}
auto failure_case(const char* suffix, int mode) -> bool {
  File file{path(suffix)};
  auto d = tavern::create_database(file.path);
  if (!d || !initial(*d)) return false;
  const auto generation = database_impl(*d).commit.generation;
  if (mode == 0) database_impl(*d).store.fail_append_after_for_test = 1;
  if (mode == 1) database_impl(*d).store.fail_data_sync_for_test = true;
  if (mode == 2) database_impl(*d).store.fail_metadata_write_for_test = true;
  if (mode == 3) database_impl(*d).store.fail_metadata_sync_for_test = true;
  const auto result = tavern::follow(*d, 3, 4);
  if (result) return false;
  if (mode < 2 && database_impl(*d).commit.generation != generation)
    return false;
  if (mode >= 2 && !database_impl(*d).commit.recovery_required) return false;
  tavern::close_database(*d);
  return consistent_after(file.path);
}
auto friend_failure_case(const char* suffix, int mode) -> bool {
  File file{path(suffix)};
  auto d = tavern::create_database(file.path);
  if (!d || !initial(*d)) return false;
  if (mode == 0) database_impl(*d).store.fail_append_after_for_test = 1;
  if (mode == 1) database_impl(*d).store.fail_data_sync_for_test = true;
  if (mode == 2) database_impl(*d).store.fail_metadata_write_for_test = true;
  if (mode == 3) database_impl(*d).store.fail_metadata_sync_for_test = true;
  const auto result = tavern::befriend(*d, 3, 4);
  if (result) return false;
  if (mode >= 2 && (!database_impl(*d).commit.recovery_required ||
                    tavern::are_friends(*d, 3, 4)))
    return false;
  tavern::close_database(*d);
  auto r = tavern::open_database(file.path);
  if (!r || !tavern::is_following(*r, 1, 2).value()) return false;
  const auto left = tavern::are_friends(*r, 3, 4);
  const auto right = tavern::are_friends(*r, 4, 3);
  if (!left || !right || *left != *right) return false;
  if (mode < 2 && *left) return false;
  tavern::close_database(*r);
  return true;
}
auto success_case() -> bool {
  File file{path("-success.db")};
  auto d = tavern::create_database(file.path);
  if (!d || !initial(*d) || !tavern::befriend(*d, 3, 4)) return false;
  const auto generation = database_impl(*d).commit.generation;
  tavern::close_database(*d);
  auto r = tavern::open_database(file.path);
  return r && database_impl(*r).commit.generation == generation &&
         tavern::are_friends(*r, 3, 4).value();
}
}  // namespace
auto main() -> int {
  return failure_case("-append.db", 0) && failure_case("-sync.db", 1) &&
                 failure_case("-write.db", 2) &&
                 failure_case("-metasync.db", 3) &&
                 friend_failure_case("-friend-append.db", 0) &&
                 friend_failure_case("-friend-sync.db", 1) &&
                 friend_failure_case("-friend-write.db", 2) &&
                 friend_failure_case("-friend-metasync.db", 3) && success_case()
             ? 0
             : 1;
}
