#include "commit.hpp"

#include <limits>
namespace tavern {
auto initialize_commit_context(PageStore& store)
    -> std::expected<CommitContext, Error> {
  const auto metadata = read_metadata(store, metadata_slot_a);
  if (!metadata) return std::unexpected(metadata.error());
  return CommitContext{metadata->root, metadata->generation,
                       metadata->committed_page_count, metadata_slot_a, false};
}
auto commit_root(PageStore& store, CommitContext& c, PageId root)
    -> std::expected<void, Error> {
  if (c.recovery_required) return std::unexpected(Error::recovery_required);
  if (c.extent > store.page_count ||
      c.generation == std::numeric_limits<std::uint64_t>::max() ||
      (root != invalid_page_id && root >= store.page_count))
    return std::unexpected(Error::commit_not_published);
  if (root != invalid_page_id) {
    const auto page = read_page(store, root);
    if (!page || (page->type != PageType::directory_leaf &&
                  page->type != PageType::directory_node))
      return std::unexpected(Error::commit_not_published);
  }
  if (root == c.root) return {};
  // Copy-on-write pages are appended or reserved before this function. Their
  // reachability becomes durable only through the following ordered protocol:
  //
  //   write replacement pages -> fsync(data file) -> write inactive metadata
  //   slot -> fsync(metadata slot/file) -> select new generation in memory
  //
  // A crash before the second fsync leaves recovery free to choose the older
  // valid slot; after it, recovery may choose the new slot. This gives
  // generation-level atomic publication, not durability for a mutation whose
  // caller received an error after an indeterminate metadata write.
  crash_if_selected_for_test(CrashPoint::before_data_sync);
  if (!sync_page_store(store))
    return std::unexpected(Error::commit_not_published);
  crash_if_selected_for_test(CrashPoint::after_data_sync);
  const PageId inactive =
      c.active_slot == metadata_slot_a ? metadata_slot_b : metadata_slot_a;
  const CommitMetadata metadata{c.generation + 1, root, store.page_count};
  const auto outcome = write_metadata_durable(store, inactive, metadata);
  if (outcome == MetadataWriteOutcome::success) {
    c.root = root;
    c.generation = metadata.generation;
    c.extent = metadata.committed_page_count;
    c.active_slot = inactive;
    return {};
  }
  if (outcome == MetadataWriteOutcome::indeterminate) {
    // The active slot might already have changed on stable storage. Continuing
    // with stale in-memory metadata could overwrite the wrong slot, so only a
    // reopen may establish the next safe commit context.
    c.recovery_required = true;
    return std::unexpected(Error::recovery_required);
  }
  return std::unexpected(Error::commit_not_published);
}
}  // namespace tavern
