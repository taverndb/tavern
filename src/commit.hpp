#pragma once
#include "page_store.hpp"
namespace tavern {
// The in-memory view of the selected metadata generation. `active_slot` names
// the slot from which it was recovered; commits publish to the other slot.
// After an indeterminate metadata write, no further operation may use this
// context (`recovery_required`) because durable publication may have happened
// even though the process could not establish that fact.
struct CommitContext {
  PageId root = invalid_page_id;
  std::uint64_t generation = 0;
  PageId extent = first_data_page;
  PageId active_slot = metadata_slot_a;
  bool recovery_required = false;
};
std::expected<CommitContext, Error> initialize_commit_context(PageStore& store);
std::expected<void, Error> commit_root(PageStore& store, CommitContext& context,
                                       PageId candidate_root);
}  // namespace tavern
