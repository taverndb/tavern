#pragma once

#include <chrono>
#include <optional>
#include <vector>

#include "database.hpp"

namespace tavern {

// A retained, ordered snapshot of pages unreachable from both valid metadata
// generations. It is owned by one Database instance and is valid only while
// every recorded identity field still matches that instance. Reservations
// remove candidates monotonically; no page is returned to this pool after a
// failed write because its contents may have changed. This is the production
// reuse mechanism, distinct from PageLivenessIndex test support.
struct FreePagePool {
  std::vector<PageId> candidates;
  std::uint64_t generation = 0;
  PageId root = invalid_page_id;
  PageId active_metadata_slot = invalid_page_id;
  PageId committed_extent = first_data_page;
  PageId physical_page_count = 0;
  const PageStore* owner_store = nullptr;
  bool invalidated = false;
};

std::expected<FreePagePool, Error> build_free_page_pool(
    const Database& database);
bool free_page_pool_matches(const FreePagePool& pool, const Database& database);
std::expected<std::optional<PageId>, Error> reserve_free_page(
    FreePagePool& pool, const Database& database);
void advance_retained_free_page_pool(FreePagePool& pool,
                                     const Database& database);
void reset_free_page_pool_scan_count_for_test();
std::size_t free_page_pool_scan_count_for_test();
std::chrono::nanoseconds free_page_pool_scan_elapsed_for_test();
std::size_t last_free_page_pool_candidate_count_for_test();

}  // namespace tavern
