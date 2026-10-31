#pragma once

#include "free_page_pool.hpp"

namespace tavern::storage_detail {

// A session has exclusive control of structural page allocation for one
// mutation. It consumes a fresh FreePagePool first, then permanently switches
// to append allocation. The latter ordering avoids mixing an uncertain reused
// page write with later assumptions that reservations remain intact.
enum class AllocationSource { reused, appended };

struct AllocationJournalEntry {
  PageId page = invalid_page_id;
  AllocationSource source = AllocationSource::appended;
};

struct PageAllocationSession {
  Database* database = nullptr;
  FreePagePool pool;
  std::vector<PageId> reserved_pages;
  bool append_phase = false;
  bool failed = false;
  bool index_backed = false;

  PageAllocationSession() = default;
  ~PageAllocationSession();
  PageAllocationSession(const PageAllocationSession&) = delete;
  PageAllocationSession& operator=(const PageAllocationSession&) = delete;
  PageAllocationSession(PageAllocationSession&& other) noexcept;
  PageAllocationSession& operator=(PageAllocationSession&& other) noexcept;

 private:
  // Records successful physical allocations in order. Production reuse uses
  // the retained pool; this journal is consumed by the non-installed
  // PageLivenessIndex reference implementation used by tests and benchmarks.
  // It is not durable recovery metadata.
  std::vector<AllocationJournalEntry> journal;

  friend std::expected<PageId, Error> allocate_encoded_page(
      PageAllocationSession&, std::span<const std::byte>);
  friend const std::vector<AllocationJournalEntry>& allocation_journal(
      const PageAllocationSession&);
  friend std::vector<AllocationJournalEntry> take_allocation_journal(
      PageAllocationSession&);
};

std::expected<PageAllocationSession, Error> begin_page_allocation_session(
    Database& database, FreePagePool&& pool);
std::expected<PageAllocationSession, Error>
begin_index_backed_page_allocation_session(Database& database,
                                           const PageLivenessIndex& index);
std::expected<PageId, Error> allocate_encoded_page(
    PageAllocationSession& session, std::span<const std::byte> encoded_page);
const std::vector<AllocationJournalEntry>& allocation_journal(
    const PageAllocationSession& session);
std::vector<AllocationJournalEntry> take_allocation_journal(
    PageAllocationSession& session);

}  // namespace tavern::storage_detail
