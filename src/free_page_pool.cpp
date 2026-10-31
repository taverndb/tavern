#include "free_page_pool.hpp"

#include <limits>

#include "database_internal.hpp"
#include "reachability.hpp"

namespace tavern {
namespace {
std::size_t free_page_pool_scan_count = 0;
std::size_t last_free_page_pool_candidate_count = 0;
std::chrono::nanoseconds free_page_pool_scan_elapsed{};
}  // namespace

auto build_free_page_pool(const Database& database)
    -> std::expected<FreePagePool, Error> {
  if (database_impl(database).commit.recovery_required)
    return std::unexpected(Error::recovery_required);
  if (database_impl(database).store.file_descriptor < 0)
    return std::unexpected(Error::invalid_page);

  ++free_page_pool_scan_count;
  const auto scan_started = std::chrono::steady_clock::now();
  // The scan includes both recoverable metadata roots. Consequently, pages
  // detached by the current mutation are not candidates until a later scan has
  // observed that neither slot can reach them.
  const auto scan = scan_reachable_pages(database_impl(database).store);
  free_page_pool_scan_elapsed +=
      std::chrono::steady_clock::now() - scan_started;
  if (!scan) return std::unexpected(scan.error());
  if (scan->unreachable_data_pages > std::numeric_limits<std::size_t>::max())
    return std::unexpected(Error::page_limit);

  try {
    FreePagePool pool{};
    pool.generation = database_impl(database).commit.generation;
    pool.root = database_impl(database).commit.root;
    pool.active_metadata_slot = database_impl(database).commit.active_slot;
    pool.committed_extent = database_impl(database).commit.extent;
    pool.physical_page_count = database_impl(database).store.page_count;
    pool.owner_store = &database_impl(database).store;
    pool.candidates.reserve(
        static_cast<std::size_t>(scan->unreachable_data_pages));
    for (PageId id = first_data_page;
         id < database_impl(database).store.page_count; ++id) {
      if (!scan->reachable[static_cast<std::size_t>(id)]) {
        if (!pool.candidates.empty() && pool.candidates.back() >= id)
          return std::unexpected(Error::invalid_page);
        pool.candidates.push_back(id);
      }
    }
    last_free_page_pool_candidate_count = pool.candidates.size();
    return pool;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::out_of_memory);
  }
}

void advance_retained_free_page_pool(FreePagePool& pool,
                                     const Database& database) {
  // Candidates were unreachable before this publication. Advancing only the
  // identity snapshot retains that conservative set; it deliberately does not
  // add pages detached by the just-published generation.
  pool.generation = database_impl(database).commit.generation;
  pool.root = database_impl(database).commit.root;
  pool.active_metadata_slot = database_impl(database).commit.active_slot;
  pool.committed_extent = database_impl(database).commit.extent;
  pool.physical_page_count = database_impl(database).store.page_count;
  pool.owner_store = &database_impl(database).store;
}

void reset_free_page_pool_scan_count_for_test() {
  free_page_pool_scan_count = 0;
  last_free_page_pool_candidate_count = 0;
  free_page_pool_scan_elapsed = {};
}
auto free_page_pool_scan_count_for_test() -> std::size_t {
  return free_page_pool_scan_count;
}
auto free_page_pool_scan_elapsed_for_test() -> std::chrono::nanoseconds {
  return free_page_pool_scan_elapsed;
}
auto last_free_page_pool_candidate_count_for_test() -> std::size_t {
  return last_free_page_pool_candidate_count;
}

auto free_page_pool_matches(const FreePagePool& pool, const Database& database)
    -> bool {
  return !pool.invalidated &&
         !database_impl(database).commit.recovery_required &&
         database_impl(database).store.file_descriptor >= 0 &&
         pool.owner_store == &database_impl(database).store &&
         pool.generation == database_impl(database).commit.generation &&
         pool.root == database_impl(database).commit.root &&
         pool.active_metadata_slot ==
             database_impl(database).commit.active_slot &&
         pool.committed_extent == database_impl(database).commit.extent &&
         pool.physical_page_count == database_impl(database).store.page_count;
}

auto reserve_free_page(FreePagePool& pool, const Database& database)
    -> std::expected<std::optional<PageId>, Error> {
  if (database_impl(database).commit.recovery_required)
    return std::unexpected(Error::recovery_required);
  if (database_impl(database).store.file_descriptor < 0 ||
      !free_page_pool_matches(pool, database))
    return std::unexpected(Error::invalid_page);
  if (pool.candidates.empty()) return std::optional<PageId>{};
  const PageId page = pool.candidates.front();
  if (page < first_data_page ||
      (pool.candidates.size() > 1 && page >= pool.candidates[1]))
    return std::unexpected(Error::invalid_page);
  pool.candidates.erase(pool.candidates.begin());
  return std::optional<PageId>{page};
}

}  // namespace tavern
