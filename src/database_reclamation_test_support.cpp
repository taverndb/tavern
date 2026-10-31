#include "database_internal.hpp"
#include "page_allocation_session.hpp"

namespace tavern {
namespace {

template <typename Mutation, typename Query>
auto mutate_with_index_reuse(Database& database, PersonId a, PersonId b,
                             bool expected_present, Mutation mutation,
                             Query query) -> std::expected<void, Error> {
  if (!database_usable_for_operation(database))
    return std::unexpected(database_unavailable_error(database));
  if (a == b) return std::unexpected(Error::self_relationship);
  if (!database_impl(database).page_liveness_shadow ||
      !database_impl(database).page_liveness_shadow->valid)
    return std::unexpected(Error::invalid_page);
  const auto present =
      query(database_impl(database).store, database_impl(database).commit.root,
            database_impl(database).commit.extent, a, b);
  if (!present) {
    database_impl(database).page_liveness_shadow.reset();
    return std::unexpected(present.error());
  }
  if (*present == expected_present) return {};

  PageId root = invalid_page_id;
  std::vector<storage_detail::AllocationJournalEntry> journal;
  {
    auto session = storage_detail::begin_index_backed_page_allocation_session(
        database, *database_impl(database).page_liveness_shadow);
    if (!session) {
      database_impl(database).page_liveness_shadow.reset();
      return std::unexpected(session.error());
    }
    const auto candidate = mutation(
        database_impl(database).store, database_impl(database).commit.root,
        database_impl(database).commit.extent, a, b);
    if (!candidate) {
      database_impl(database).page_liveness_shadow.reset();
      return std::unexpected(candidate.error());
    }
    root = *candidate;
    journal = storage_detail::take_allocation_journal(*session);
  }
  if (root == database_impl(database).commit.root) {
    if (!journal.empty()) database_impl(database).page_liveness_shadow.reset();
    return {};
  }
  const PageId overwritten_slot =
      database_impl(database).commit.active_slot == metadata_slot_a
          ? metadata_slot_b
          : metadata_slot_a;
  const auto committed = commit_root(database_impl(database).store,
                                     database_impl(database).commit, root);
  if (!committed) {
    database_impl(database).page_liveness_shadow.reset();
    return std::unexpected(committed.error());
  }
  const auto updated = update_page_liveness_index(
      *database_impl(database).page_liveness_shadow,
      database_impl(database).store, overwritten_slot, root,
      database_impl(database).commit.extent,
      database_impl(database).commit.generation, journal);
  if (!updated) {
    database_impl(database).page_liveness_shadow.reset();
    return {};
  }
  database_impl(database).page_liveness_shadow_diagnostics = *updated;
  return {};
}

}  // namespace

auto initialize_page_liveness_shadow_for_test(Database& database)
    -> std::expected<void, Error> {
  if (!database_usable_for_operation(database))
    return std::unexpected(database_unavailable_error(database));
  const auto index = build_page_liveness_index(database_impl(database).store);
  if (!index) return std::unexpected(index.error());
  database_impl(database).page_liveness_shadow =
      std::make_unique<PageLivenessIndex>(*index);
  database_impl(database).page_liveness_shadow_diagnostics = {};
  return {};
}

auto page_liveness_shadow_candidates_for_test(const Database& database)
    -> std::expected<std::vector<PageId>, Error> {
  if (!database_usable_for_operation(database) ||
      !database_impl(database).page_liveness_shadow ||
      !database_impl(database).page_liveness_shadow->valid ||
      database_impl(database).page_liveness_shadow->physical_page_count !=
          database_impl(database).store.page_count) {
    return std::unexpected(Error::invalid_page);
  }
  const auto& index = *database_impl(database).page_liveness_shadow;
  if (index.protected_roots.size() != 2)
    return std::unexpected(Error::invalid_metadata);
  PageId previous = invalid_page_id;
  for (const PageLivenessRoot& root : index.protected_roots) {
    const auto metadata =
        read_metadata(database_impl(database).store, root.metadata_slot);
    if (!metadata || metadata->root != root.root ||
        metadata->committed_page_count != root.extent ||
        metadata->generation != root.generation)
      return std::unexpected(Error::invalid_metadata);
  }
  for (const PageId page : index.reclaimable) {
    if (page < first_data_page ||
        page >= database_impl(database).store.page_count ||
        (previous != invalid_page_id && page <= previous) || index.live(page))
      return std::unexpected(Error::invalid_page);
    previous = page;
  }
  try {
    return index.reclaimable;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::out_of_memory);
  }
}

auto follow_with_index_reuse_for_test(Database& database, PersonId a,
                                      PersonId b)
    -> std::expected<void, Error> {
  return mutate_with_index_reuse(database, a, b, true, persistent_follow,
                                 persistent_is_following);
}

}  // namespace tavern
