#include <unistd.h>

#include <array>
#include <filesystem>
#include <unordered_set>

#include "database_internal.hpp"
#include "reachability.hpp"

namespace {

auto path(const char* suffix) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         (std::string("tavern-incremental-follow-") +
          std::to_string(static_cast<long long>(getpid())) + suffix);
}

auto exact(const tavern::Database& database) -> bool {
  const auto scan = tavern::scan_reachable_pages(database_impl(database).store);
  const auto candidates =
      tavern::page_liveness_shadow_candidates_for_test(database);
  if (!scan || !candidates || !database_impl(database).page_liveness_shadow)
    return false;
  std::vector<tavern::PageId> expected;
  for (tavern::PageId page = tavern::first_data_page;
       page < database_impl(database).store.page_count; ++page)
    if (!scan->reachable[static_cast<std::size_t>(page)])
      expected.push_back(page);
  if (*candidates != expected) return false;
  for (const tavern::PageId page : *candidates)
    if (database_impl(database)
            .page_liveness_shadow
            ->incoming_references[static_cast<std::size_t>(page)] != 0)
      return false;
  const auto roots =
      tavern::scan_protected_directories(database_impl(database).store);
  return roots && roots->roots.size() == 2;
}

auto successive_commits_reuse_without_scans() -> bool {
  const auto file = path("-success.db");
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  if (!database || !tavern::initialize_page_liveness_shadow_for_test(*database))
    return false;

  tavern::reset_reachability_scan_count_for_test();
  if (!tavern::follow_with_index_reuse_for_test(*database, 1, 2) ||
      database_impl(*database)
              .page_liveness_shadow_diagnostics.appended_pages == 0 ||
      !tavern::follow_with_index_reuse_for_test(*database, 3, 4) ||
      !tavern::follow_with_index_reuse_for_test(*database, 1, 5) ||
      tavern::reachability_scan_count_for_test() != 0 ||
      !database_impl(*database).page_liveness_shadow ||
      database_impl(*database).page_liveness_shadow_diagnostics.reused_pages ==
          0 ||
      database_impl(*database)
              .page_liveness_shadow_diagnostics.journaled_reclaimable_pages ==
          0) {
    return false;
  }
  const auto generation = database_impl(*database).commit.generation;
  if (!tavern::follow_with_index_reuse_for_test(*database, 1, 5) ||
      database_impl(*database).commit.generation != generation ||
      tavern::reachability_scan_count_for_test() != 0 || !exact(*database)) {
    return false;
  }

  const auto active =
      tavern::read_metadata(database_impl(*database).store,
                            database_impl(*database).commit.active_slot);
  const auto inactive = tavern::read_metadata(
      database_impl(*database).store,
      database_impl(*database).commit.active_slot == tavern::metadata_slot_a
          ? tavern::metadata_slot_b
          : tavern::metadata_slot_a);
  if (!active || !inactive ||
      active->generation != database_impl(*database).commit.generation ||
      active->committed_page_count != database_impl(*database).commit.extent) {
    return false;
  }
  if (!tavern::close_database(*database)) return false;
  auto reopened = tavern::open_database(file);
  const bool recovered =
      reopened && !database_impl(*reopened).page_liveness_shadow &&
      tavern::initialize_page_liveness_shadow_for_test(*reopened) &&
      tavern::is_following(*reopened, 1, 2).value_or(false) &&
      tavern::is_following(*reopened, 3, 4).value_or(false) &&
      tavern::is_following(*reopened, 1, 5).value_or(false) &&
      exact(*reopened) && tavern::close_database(*reopened);
  std::filesystem::remove(file);
  return recovered;
}

auto reuse_then_append_fallback() -> bool {
  const auto file = path("-reuse-append.db");
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  const std::array<std::byte, 1> payload{std::byte{0x6B}};
  if (!database ||
      !tavern::append_page(database_impl(*database).store,
                           tavern::PageType::data, payload) ||
      !tavern::initialize_page_liveness_shadow_for_test(*database) ||
      database_impl(*database).page_liveness_shadow->reclaimable.size() != 1 ||
      !tavern::follow_with_index_reuse_for_test(*database, 1, 2) ||
      database_impl(*database).page_liveness_shadow_diagnostics.reused_pages ==
          0 ||
      database_impl(*database)
              .page_liveness_shadow_diagnostics.appended_pages == 0 ||
      !exact(*database)) {
    return false;
  }
  const bool closed = tavern::close_database(*database).has_value();
  std::filesystem::remove(file);
  return closed;
}

auto failures_invalidate_and_rebuild() -> bool {
  const auto file = path("-failures.db");
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  if (!database || tavern::follow_with_index_reuse_for_test(*database, 1, 2))
    return false;
  // The opt-in mode must be explicitly reconstructed before its first use.
  if (!tavern::follow(*database, 1, 2) ||
      !tavern::initialize_page_liveness_shadow_for_test(*database) ||
      !tavern::follow_with_index_reuse_for_test(*database, 3, 4) ||
      database_impl(*database).page_liveness_shadow_diagnostics.reused_pages ==
          0) {
    return false;
  }
  database_impl(*database).store.fail_reusable_write_after_for_test = 1;
  if (tavern::follow_with_index_reuse_for_test(*database, 5, 6) ||
      database_impl(*database).page_liveness_shadow ||
      tavern::follow_with_index_reuse_for_test(*database, 5, 6) ||
      !tavern::initialize_page_liveness_shadow_for_test(*database) ||
      !exact(*database)) {
    return false;
  }
  database_impl(*database).store.fail_data_sync_for_test = true;
  if (tavern::follow_with_index_reuse_for_test(*database, 5, 6) ||
      database_impl(*database).page_liveness_shadow) {
    return false;
  }
  database_impl(*database).store.fail_data_sync_for_test = false;
  if (!tavern::initialize_page_liveness_shadow_for_test(*database))
    return false;
  database_impl(*database).store.fail_metadata_write_for_test = true;
  if (tavern::follow_with_index_reuse_for_test(*database, 5, 6) ||
      database_impl(*database).page_liveness_shadow ||
      !database_impl(*database).commit.recovery_required ||
      !tavern::close_database(*database)) {
    return false;
  }
  auto reopened = tavern::open_database(file);
  const bool recovered =
      reopened && tavern::initialize_page_liveness_shadow_for_test(*reopened) &&
      !tavern::is_following(*reopened, 5, 6).value_or(true) &&
      exact(*reopened) && tavern::close_database(*reopened);
  std::filesystem::remove(file);
  return recovered;
}

auto append_failure() -> bool {
  const auto file = path("-append.db");
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  if (!database || !tavern::initialize_page_liveness_shadow_for_test(*database))
    return false;
  database_impl(*database).store.fail_append_after_for_test = 0;
  const bool failed =
      !tavern::follow_with_index_reuse_for_test(*database, 1, 2) &&
      !database_impl(*database).page_liveness_shadow;
  const bool closed = tavern::close_database(*database).has_value();
  std::filesystem::remove(file);
  return failed && closed;
}

}  // namespace

auto main() -> int {
  return successive_commits_reuse_without_scans() &&
                 reuse_then_append_fallback() &&
                 failures_invalidate_and_rebuild() && append_failure()
             ? 0
             : 1;
}
