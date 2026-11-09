#include <unistd.h>

#include <algorithm>
#include <array>
#include <filesystem>

#include "database_internal.hpp"
#include "free_page_pool.hpp"
#include "reachability.hpp"

namespace {

auto path(const char* suffix) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         (std::string("tavern-retained-pool-") +
          std::to_string(static_cast<long long>(getpid())) + suffix);
}

auto add_candidates(tavern::Database& database, int count = 32) -> bool {
  const std::array<std::byte, 1> payload{std::byte{0x5A}};
  for (int index = 0; index < count; ++index)
    if (!tavern::append_page(database_impl(database).store,
                             tavern::PageType::data, payload))
      return false;
  return true;
}

auto cached_commits_and_noops() -> bool {
  const auto file = path("-cache.db");
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  if (!database || !tavern::follow(*database, 1, 2) ||
      !add_candidates(*database))
    return false;
  const auto initial = tavern::build_free_page_pool(*database);
  if (!initial || initial->candidates.empty()) return false;
  tavern::reset_free_page_pool_scan_count_for_test();
  const tavern::PageId pages = database_impl(*database).store.page_count;
  if (!tavern::follow(*database, 3, 4) ||
      tavern::free_page_pool_scan_count_for_test() != 1 ||
      !database_impl(*database).retained_free_page_pool ||
      database_impl(*database).store.page_count != pages)
    return false;
  const auto first_remaining =
      database_impl(*database).retained_free_page_pool->candidates;
  if (first_remaining.empty() ||
      first_remaining.size() >= initial->candidates.size() ||
      !std::equal(first_remaining.begin(), first_remaining.end(),
                  initial->candidates.end() -
                      static_cast<std::ptrdiff_t>(first_remaining.size())))
    return false;
  if (!tavern::follow(*database, 3, 4) ||
      tavern::free_page_pool_scan_count_for_test() != 1 ||
      database_impl(*database).retained_free_page_pool->candidates !=
          first_remaining)
    return false;
  if (!tavern::follow(*database, 5, 6) ||
      tavern::free_page_pool_scan_count_for_test() != 1 ||
      !database_impl(*database).retained_free_page_pool ||
      database_impl(*database).retained_free_page_pool->candidates.size() >=
          first_remaining.size() ||
      database_impl(*database).store.page_count != pages)
    return false;
  const auto reachable =
      tavern::scan_reachable_pages(database_impl(*database).store);
  bool newly_unreachable = false;
  if (reachable)
    for (tavern::PageId id = tavern::first_data_page;
         id < database_impl(*database).store.page_count; ++id) {
      const bool originally_free =
          std::find(initial->candidates.begin(), initial->candidates.end(),
                    id) != initial->candidates.end();
      newly_unreachable =
          newly_unreachable ||
          (!reachable->reachable[static_cast<std::size_t>(id)] &&
           !originally_free);
    }
  bool exhausted = false;
  for (tavern::PersonId id = 100;
       id < 140 && database_impl(*database).retained_free_page_pool; ++id) {
    if (!tavern::follow(*database, id, id + 1000)) return false;
    exhausted = !database_impl(*database).retained_free_page_pool;
  }
  if (!exhausted || tavern::free_page_pool_scan_count_for_test() != 1 ||
      !tavern::follow(*database, 200, 1200) ||
      tavern::free_page_pool_scan_count_for_test() != 2)
    return false;
  const bool result =
      reachable && newly_unreachable && tavern::close_database(*database);
  std::filesystem::remove(file);
  return result;
}

auto exhaustion_failure_close_and_move() -> bool {
  const auto file = path("-lifecycle.db");
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  if (!database || !tavern::follow(*database, 1, 2) ||
      !add_candidates(*database, 8))
    return false;
  tavern::reset_free_page_pool_scan_count_for_test();
  if (!tavern::follow(*database, 3, 4) ||
      !database_impl(*database).retained_free_page_pool)
    return false;
  database_impl(*database).store.fail_reusable_write_after_for_test = 0;
  if (tavern::follow(*database, 5, 6) ||
      database_impl(*database).retained_free_page_pool)
    return false;
  database_impl(*database).store.fail_reusable_write_after_for_test = -1;
  if (!tavern::follow(*database, 5, 6) ||
      tavern::free_page_pool_scan_count_for_test() != 2)
    return false;
  tavern::Database moved(std::move(*database));
  if (database_impl(moved).retained_free_page_pool) return false;
  if (!tavern::follow(moved, 7, 8) ||
      tavern::free_page_pool_scan_count_for_test() != 3 ||
      !tavern::close_database(moved))
    return false;
  auto reopened = tavern::open_database(file);
  if (!reopened || database_impl(*reopened).retained_free_page_pool)
    return false;
  const auto before = tavern::free_page_pool_scan_count_for_test();
  if (!tavern::follow(*reopened, 9, 10) ||
      tavern::free_page_pool_scan_count_for_test() != before + 1)
    return false;
  database_impl(*reopened).store.fail_metadata_write_for_test = true;
  const bool metadata_failure =
      !tavern::follow(*reopened, 11, 12) &&
      database_impl(*reopened).commit.recovery_required &&
      !database_impl(*reopened).retained_free_page_pool;
  const auto closed = tavern::close_database(*reopened);
  std::filesystem::remove(file);
  return metadata_failure && closed.has_value();
}

}  // namespace

auto main() -> int {
  return cached_commits_and_noops() && exhaustion_failure_close_and_move() ? 0
                                                                           : 1;
}
