#include <unistd.h>

#include <array>
#include <filesystem>

#include "database_internal.hpp"
#include "reachability.hpp"
namespace {
auto path(const char* s) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         (std::string("tavern-befriend-reuse-") +
          std::to_string(static_cast<long long>(getpid())) + s);
}
auto candidates(tavern::Database& d) -> bool {
  const std::array<std::byte, 1> p{std::byte{0x2A}};
  for (int i = 0; i < 4; ++i)
    if (!tavern::append_page(database_impl(d).store, tavern::PageType::data, p))
      return false;
  return true;
}
auto empty() -> bool {
  const auto p = path("-empty.db");
  std::filesystem::remove(p);
  auto d = tavern::create_database(p);
  const bool ok = d && tavern::befriend(*d, 1, 2) &&
                  tavern::are_friends(*d, 1, 2).value_or(false) &&
                  tavern::close_database(*d);
  std::filesystem::remove(p);
  return ok;
}
auto success() -> bool {
  const auto p = path("-success.db");
  std::filesystem::remove(p);
  auto d = tavern::create_database(p);
  if (!d || !tavern::follow(*d, 1, 2) || !tavern::follow(*d, 4, 5) ||
      !candidates(*d))
    return false;
  const auto scan = tavern::scan_reachable_pages(database_impl(*d).store);
  const auto extent = database_impl(*d).commit.extent;
  const auto pages = database_impl(*d).store.page_count;
  bool below = false;
  if (scan)
    for (tavern::PageId id = tavern::first_data_page; id < pages; ++id)
      below = below ||
              (!scan->reachable[static_cast<std::size_t>(id)] && id < extent);
  if (!scan || !below || !tavern::befriend(*d, 6, 7) ||
      database_impl(*d).store.page_count != pages ||
      !tavern::are_friends(*d, 6, 7).value_or(false) ||
      !tavern::are_friends(*d, 7, 6).value_or(false) ||
      !tavern::is_following(*d, 1, 2).value_or(false) ||
      !tavern::befriend(*d, 6, 7))
    return false;
  const auto roots =
      tavern::scan_protected_directories(database_impl(*d).store);
  const auto generation = database_impl(*d).commit.generation;
  if (!roots || roots->roots.size() != 2 || !tavern::close_database(*d))
    return false;
  auto r = tavern::open_database(p);
  const bool ok = r && database_impl(*r).commit.generation == generation &&
                  tavern::are_friends(*r, 6, 7).value_or(false) &&
                  tavern::are_friends(*r, 7, 6).value_or(false) &&
                  tavern::is_following(*r, 1, 2).value_or(false);
  if (r) (void)tavern::close_database(*r);
  std::filesystem::remove(p);
  return ok;
}
auto failures() -> bool {
  const auto p = path("-failure.db");
  std::filesystem::remove(p);
  auto d = tavern::create_database(p);
  if (!d || !tavern::follow(*d, 1, 2) || !candidates(*d)) return false;
  const auto generation = database_impl(*d).commit.generation;
  database_impl(*d).store.fail_reusable_write_after_for_test = 1;
  if (tavern::befriend(*d, 3, 4) ||
      database_impl(*d).commit.generation != generation ||
      tavern::are_friends(*d, 3, 4).value_or(true) ||
      tavern::are_friends(*d, 4, 3).value_or(true))
    return false;
  if (!tavern::close_database(*d)) return false;
  auto r = tavern::open_database(p);
  if (!r || tavern::are_friends(*r, 3, 4).value_or(true)) return false;
  database_impl(*r).store.fail_metadata_write_for_test = true;
  const bool failed =
      !tavern::befriend(*r, 3, 4) && database_impl(*r).commit.recovery_required;
  if (!tavern::close_database(*r)) return false;
  auto final = tavern::open_database(p);
  const bool ok = final && !tavern::are_friends(*final, 3, 4).value_or(true) &&
                  !tavern::are_friends(*final, 4, 3).value_or(true);
  if (final) (void)tavern::close_database(*final);
  std::filesystem::remove(p);
  return failed && ok;
}
}  // namespace
auto main() -> int { return empty() && success() && failures() ? 0 : 1; }
