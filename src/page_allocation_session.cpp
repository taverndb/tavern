#include "page_allocation_session.hpp"

#include <algorithm>

#include "database_internal.hpp"
#include "page_store_internal.hpp"

namespace tavern::storage_detail {
namespace {

void release(PageAllocationSession& session) {
  if (session.database) {
    database_impl(*session.database).allocation_session_active = false;
    database_impl(*session.database).store.allocation_session_active = false;
    database_impl(*session.database).store.session_allocator = nullptr;
    database_impl(*session.database).store.session_allocator_context = nullptr;
    session.database = nullptr;
  }
}

auto fail(PageAllocationSession& session, Error error)
    -> std::expected<PageId, Error> {
  // A failed reusable overwrite can have changed the page partially. Discard
  // this pool rather than return the reservation to a later mutation. With the
  // old metadata still protected, recovery remains safe; the damaged page was
  // selected only because it was unreachable from both protected roots.
  session.failed = true;
  session.pool.invalidated = true;
  if (session.index_backed && session.database)
    database_impl(*session.database).page_liveness_shadow.reset();
  return std::unexpected(error);
}

auto allocate_from_session(void* context,
                           std::span<const std::byte> encoded_page)
    -> std::expected<PageId, Error> {
  return allocate_encoded_page(*static_cast<PageAllocationSession*>(context),
                               encoded_page);
}

}  // namespace

PageAllocationSession::~PageAllocationSession() { release(*this); }

PageAllocationSession::PageAllocationSession(
    PageAllocationSession&& other) noexcept
    : database(other.database),
      pool(std::move(other.pool)),
      reserved_pages(std::move(other.reserved_pages)),
      append_phase(other.append_phase),
      failed(other.failed),
      index_backed(other.index_backed),
      journal(std::move(other.journal)) {
  if (database &&
      database_impl(*database).store.session_allocator_context == &other)
    database_impl(*database).store.session_allocator_context = this;
  other.database = nullptr;
}

auto PageAllocationSession::operator=(PageAllocationSession&& other) noexcept
    -> PageAllocationSession& {
  if (this != &other) {
    release(*this);
    database = other.database;
    pool = std::move(other.pool);
    reserved_pages = std::move(other.reserved_pages);
    journal = std::move(other.journal);
    append_phase = other.append_phase;
    failed = other.failed;
    index_backed = other.index_backed;
    if (database &&
        database_impl(*database).store.session_allocator_context == &other)
      database_impl(*database).store.session_allocator_context = this;
    other.database = nullptr;
  }
  return *this;
}

auto begin_index_backed_page_allocation_session(Database& database,
                                                const PageLivenessIndex& index)
    -> std::expected<PageAllocationSession, Error> {
  if (database_impl(database).commit.recovery_required ||
      database_impl(database).store.file_descriptor < 0 ||
      database_impl(database).allocation_session_active ||
      database_impl(database).store.allocation_session_active ||
      database_impl(database).page_liveness_shadow.get() != &index ||
      !index.valid ||
      index.physical_page_count != database_impl(database).store.page_count ||
      index.incoming_references.size() !=
          static_cast<std::size_t>(database_impl(database).store.page_count) ||
      index.protected_roots.size() != 2) {
    return std::unexpected(Error::invalid_page);
  }
  try {
    FreePagePool pool{};
    pool.generation = database_impl(database).commit.generation;
    pool.root = database_impl(database).commit.root;
    pool.active_metadata_slot = database_impl(database).commit.active_slot;
    pool.committed_extent = database_impl(database).commit.extent;
    pool.physical_page_count = database_impl(database).store.page_count;
    pool.owner_store = &database_impl(database).store;
    pool.candidates = index.reclaimable;
    PageId previous = invalid_page_id;
    bool active_root_matches = false;
    bool saw_slot_a = false;
    bool saw_slot_b = false;
    for (const PageLivenessRoot& root : index.protected_roots) {
      if (root.metadata_slot == metadata_slot_a)
        saw_slot_a = true;
      else if (root.metadata_slot == metadata_slot_b)
        saw_slot_b = true;
      else
        return std::unexpected(Error::invalid_metadata);
      const auto metadata =
          read_metadata(database_impl(database).store, root.metadata_slot);
      if (!metadata || metadata->root != root.root ||
          metadata->committed_page_count != root.extent ||
          metadata->generation != root.generation) {
        return std::unexpected(Error::invalid_metadata);
      }
      active_root_matches =
          active_root_matches ||
          (root.metadata_slot == database_impl(database).commit.active_slot &&
           root.root == database_impl(database).commit.root &&
           root.extent == database_impl(database).commit.extent &&
           root.generation == database_impl(database).commit.generation);
    }
    if (!saw_slot_a || !saw_slot_b || !active_root_matches)
      return std::unexpected(Error::invalid_metadata);
    for (const PageId page : pool.candidates) {
      if (page < first_data_page ||
          page >= database_impl(database).store.page_count ||
          (previous != invalid_page_id && page <= previous) ||
          index.incoming_references[static_cast<std::size_t>(page)] != 0)
        return std::unexpected(Error::invalid_page);
      previous = page;
    }
    auto session = begin_page_allocation_session(database, std::move(pool));
    if (!session) return std::unexpected(session.error());
    session->index_backed = true;
    return session;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::out_of_memory);
  }
}

auto begin_page_allocation_session(Database& database, FreePagePool&& pool)
    -> std::expected<PageAllocationSession, Error> {
  if (database_impl(database).commit.recovery_required)
    return std::unexpected(Error::recovery_required);
  if (database_impl(database).store.file_descriptor < 0 ||
      database_impl(database).allocation_session_active ||
      database_impl(database).store.allocation_session_active ||
      !free_page_pool_matches(pool, database))
    return std::unexpected(Error::invalid_page);
  try {
    PageAllocationSession session{};
    session.database = &database;
    session.pool = std::move(pool);
    session.reserved_pages.reserve(session.pool.candidates.size());
    // The hook is scoped to this movable object. It centralizes all structural
    // allocations so directory and adjacency writers cannot bypass a reserved
    // page while a reuse mutation is in progress.
    database_impl(database).allocation_session_active = true;
    database_impl(database).store.allocation_session_active = true;
    database_impl(database).store.session_allocator = allocate_from_session;
    database_impl(database).store.session_allocator_context = &session;
    return session;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::out_of_memory);
  }
}

auto allocate_encoded_page(PageAllocationSession& session,
                           std::span<const std::byte> encoded_page)
    -> std::expected<PageId, Error> {
  if (!session.database || session.failed ||
      !database_impl(*session.database).allocation_session_active ||
      !database_impl(*session.database).store.allocation_session_active)
    return std::unexpected(Error::invalid_page);
  const auto record_pending =
      [&](PageId page, AllocationSource source) -> std::expected<void, Error> {
    if (std::find_if(session.journal.begin(), session.journal.end(),
                     [page](const AllocationJournalEntry& entry) -> bool {
                       return entry.page == page;
                     }) != session.journal.end()) {
      return std::unexpected(Error::invalid_page);
    }
    try {
      session.journal.push_back({page, source});
      return {};
    } catch (const std::bad_alloc&) {
      return std::unexpected(Error::out_of_memory);
    }
  };
  if (!session.append_phase) {
    if (!free_page_pool_matches(session.pool, *session.database))
      return fail(session, Error::invalid_page);
    const auto reserved = reserve_free_page(session.pool, *session.database);
    if (!reserved) return fail(session, reserved.error());
    if (*reserved) {
      const PageId page = **reserved;
      if (std::find(session.reserved_pages.begin(),
                    session.reserved_pages.end(),
                    page) != session.reserved_pages.end()) {
        return fail(session, Error::invalid_page);
      }
      try {
        session.reserved_pages.push_back(page);
      } catch (const std::bad_alloc&) {
        return fail(session, Error::out_of_memory);
      }
      const auto recorded = record_pending(page, AllocationSource::reused);
      if (!recorded) return fail(session, recorded.error());
      const auto written = overwrite_reserved_unreachable_page(
          database_impl(*session.database).store, page, encoded_page);
      if (!written) {
        session.journal.pop_back();
        return fail(session, written.error());
      }
      return page;
    }
    // Once the scanned candidates are exhausted, append only. This avoids a
    // later allocation observing a pool state changed by a preceding reuse.
    session.append_phase = true;
  }
  const PageId page = database_impl(*session.database).store.page_count;
  const auto recorded = record_pending(page, AllocationSource::appended);
  if (!recorded) return fail(session, recorded.error());
  const auto appended =
      append_encoded_page(database_impl(*session.database).store, encoded_page);
  if (!appended || *appended != page) {
    session.journal.pop_back();
    return fail(session, appended ? Error::invalid_page : appended.error());
  }
  return *appended;
}

auto allocation_journal(const PageAllocationSession& session)
    -> const std::vector<AllocationJournalEntry>& {
  return session.journal;
}

auto take_allocation_journal(PageAllocationSession& session)
    -> std::vector<AllocationJournalEntry> {
  return std::move(session.journal);
}

}  // namespace tavern::storage_detail
