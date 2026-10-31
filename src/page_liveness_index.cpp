#include "page_liveness_index.hpp"

#include <algorithm>
#include <limits>

#include "adjacency_pages.hpp"
#include "adjacency_tree.hpp"
#include "page_allocation_session.hpp"

namespace tavern {
namespace {

auto validate_root(const PageStore& store, PageId root, PageId extent)
    -> std::expected<void, Error> {
  if (extent < first_data_page || extent > store.page_count ||
      (root != invalid_page_id && (root < first_data_page || root >= extent)))
    return std::unexpected(Error::invalid_metadata);
  return traverse_directory(
      store, root, extent,
      [](PageId, PageType) -> std::expected<void, Error> {
        return std::expected<void, Error>{};
      },
      [&](const PatronRecord& record) -> std::expected<void, Error> {
        for (const AdjacencyDescriptor& descriptor :
             {record.following, record.followers, record.friends}) {
          const auto traversed = traverse_adjacency_pages(
              store, descriptor, extent,
              [](PageId, PageType) -> std::expected<void, Error> {
                return std::expected<void, Error>{};
              });
          if (!traversed) return traversed;
        }
        return std::expected<void, Error>{};
      });
}

auto outgoing_references(const PageStore& store, PageId page, PageId extent)
    -> std::expected<std::vector<PageId>, Error> {
  const auto raw = read_page(store, page);
  if (!raw) return std::unexpected(raw.error());
  if (raw->type == PageType::directory_node ||
      raw->type == PageType::directory_leaf)
    return directory_outgoing_references(store, page, extent);
  if (raw->type == PageType::adjacency_reference) {
    const auto children = read_adjacency_reference(store, page);
    if (!children) return std::unexpected(children.error());
    std::vector<PageId> references = *children;
    std::sort(references.begin(), references.end());
    references.erase(std::unique(references.begin(), references.end()),
                     references.end());
    return references;
  }
  if (raw->type == PageType::adjacency_leaf) {
    const auto leaf = read_adjacency_leaf(store, page);
    if (!leaf) return std::unexpected(leaf.error());
    return std::vector<PageId>{};
  }
  return std::unexpected(Error::invalid_page);
}

auto add_reference(PageLivenessIndex& index, const PageStore& store,
                   PageId page, PageId extent, std::size_t* activated = nullptr)
    -> std::expected<void, Error> {
  if (page < first_data_page || page >= extent ||
      page >= index.incoming_references.size())
    return std::unexpected(Error::invalid_page);
  std::vector<std::pair<PageId, PageId>> pending{{page, extent}};
  while (!pending.empty()) {
    const auto [current, current_extent] = pending.back();
    pending.pop_back();
    auto& count = index.incoming_references[static_cast<std::size_t>(current)];
    if (count == std::numeric_limits<std::uint32_t>::max())
      return std::unexpected(Error::page_limit);
    if (count++ != 0) continue;
    if (activated) ++*activated;
    const auto references = outgoing_references(store, current, current_extent);
    if (!references) return std::unexpected(references.error());
    for (const PageId child : *references) {
      if (child < first_data_page || child >= current_extent ||
          child >= index.incoming_references.size())
        return std::unexpected(Error::invalid_page);
      pending.emplace_back(child, current_extent);
    }
  }
  return {};
}

auto remove_reference(PageLivenessIndex& index, const PageStore& store,
                      PageId page, PageId extent,
                      std::size_t* deactivated = nullptr)
    -> std::expected<void, Error> {
  if (page < first_data_page || page >= extent ||
      page >= index.incoming_references.size())
    return std::unexpected(Error::invalid_page);
  std::vector<std::pair<PageId, PageId>> pending{{page, extent}};
  while (!pending.empty()) {
    const auto [current, current_extent] = pending.back();
    pending.pop_back();
    auto& count = index.incoming_references[static_cast<std::size_t>(current)];
    if (count == 0) return std::unexpected(Error::invalid_page);
    if (--count != 0) continue;
    if (deactivated) ++*deactivated;
    const auto references = outgoing_references(store, current, current_extent);
    if (!references) return std::unexpected(references.error());
    for (const PageId child : *references) {
      if (child < first_data_page || child >= current_extent ||
          child >= index.incoming_references.size())
        return std::unexpected(Error::invalid_page);
      pending.emplace_back(child, current_extent);
    }
  }
  return {};
}

void refresh_reclaimable(PageLivenessIndex& index) {
  index.reclaimable.clear();
  for (PageId page = first_data_page; page < index.physical_page_count; ++page)
    if (index.incoming_references[static_cast<std::size_t>(page)] == 0)
      index.reclaimable.push_back(page);
}

auto register_allocations(
    PageLivenessIndex& index, const PageStore& store,
    std::span<const storage_detail::AllocationJournalEntry> journal)
    -> std::expected<void, Error> {
  const PageId previous_physical_count = index.physical_page_count;
  if (store.page_count < previous_physical_count ||
      store.page_count > std::numeric_limits<std::size_t>::max())
    return std::unexpected(Error::page_limit);
  try {
    index.incoming_references.resize(
        static_cast<std::size_t>(store.page_count));
    std::vector<bool> seen(static_cast<std::size_t>(store.page_count), false);
    for (const auto& entry : journal) {
      if (entry.page < first_data_page || entry.page >= store.page_count ||
          seen[static_cast<std::size_t>(entry.page)])
        return std::unexpected(Error::invalid_page);
      seen[static_cast<std::size_t>(entry.page)] = true;
      const bool reused =
          entry.source == storage_detail::AllocationSource::reused;
      if (reused != (entry.page < previous_physical_count) ||
          index.incoming_references[static_cast<std::size_t>(entry.page)] != 0)
        return std::unexpected(Error::invalid_page);
    }
    for (PageId page = previous_physical_count; page < store.page_count; ++page)
      if (!seen[static_cast<std::size_t>(page)])
        return std::unexpected(Error::invalid_page);
    index.physical_page_count = store.page_count;
    return {};
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::out_of_memory);
  }
}

}  // namespace

auto PageLivenessIndex::live(PageId page) const -> bool {
  return page >= first_data_page && page < incoming_references.size() &&
         incoming_references[static_cast<std::size_t>(page)] != 0;
}

auto PageLivenessIndex::memory_bytes() const -> std::size_t {
  return incoming_references.capacity() * sizeof(std::uint32_t) +
         protected_roots.capacity() * sizeof(PageLivenessRoot) +
         reclaimable.capacity() * sizeof(PageId);
}

auto build_page_liveness_index(const PageStore& store)
    -> std::expected<PageLivenessIndex, Error> {
  if (store.file_descriptor < 0 || store.page_count < first_data_page)
    return std::unexpected(Error::invalid_page);
  try {
    PageLivenessIndex index;
    index.physical_page_count = store.page_count;
    index.incoming_references.resize(
        static_cast<std::size_t>(store.page_count));
    for (const PageId slot : {metadata_slot_a, metadata_slot_b}) {
      const auto metadata = read_metadata(store, slot);
      if (!metadata) continue;
      if (metadata->committed_page_count < first_data_page ||
          metadata->committed_page_count > store.page_count ||
          (metadata->root != invalid_page_id &&
           metadata->root >= metadata->committed_page_count))
        return std::unexpected(Error::invalid_metadata);
      index.protected_roots.push_back({slot, metadata->root,
                                       metadata->committed_page_count,
                                       metadata->generation});
    }
    if (index.protected_roots.empty())
      return std::unexpected(Error::invalid_metadata);
    for (const PageLivenessRoot& root : index.protected_roots) {
      const auto valid = validate_root(store, root.root, root.extent);
      if (!valid) return std::unexpected(valid.error());
    }
    for (const PageLivenessRoot& root : index.protected_roots)
      if (root.root != invalid_page_id) {
        const auto added = add_reference(index, store, root.root, root.extent);
        if (!added) return std::unexpected(added.error());
      }
    refresh_reclaimable(index);
    return index;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::out_of_memory);
  }
}

auto simulate_page_liveness_root_replacement(PageLivenessIndex& index,
                                             const PageStore& store,
                                             PageId overwritten_slot,
                                             PageId candidate_root,
                                             PageId candidate_extent)
    -> std::expected<void, Error> {
  try {
    PageLivenessIndex next = index;
    const auto position =
        std::find_if(next.protected_roots.begin(), next.protected_roots.end(),
                     [&](const PageLivenessRoot& root) -> bool {
                       return root.metadata_slot == overwritten_slot;
                     });
    if (position == next.protected_roots.end())
      return std::unexpected(Error::invalid_metadata);
    const auto valid = validate_root(store, candidate_root, candidate_extent);
    if (!valid) return std::unexpected(valid.error());
    if (candidate_root != invalid_page_id) {
      const auto added =
          add_reference(next, store, candidate_root, candidate_extent);
      if (!added) return std::unexpected(added.error());
    }
    const PageLivenessRoot replaced = *position;
    if (replaced.root != invalid_page_id) {
      const auto removed =
          remove_reference(next, store, replaced.root, replaced.extent);
      if (!removed) return std::unexpected(removed.error());
    }
    position->root = candidate_root;
    position->extent = candidate_extent;
    refresh_reclaimable(next);
    index = std::move(next);
    return {};
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::out_of_memory);
  }
}

auto update_page_liveness_index(
    PageLivenessIndex& index, const PageStore& store, PageId overwritten_slot,
    PageId candidate_root, PageId candidate_extent,
    std::uint64_t candidate_generation,
    std::span<const storage_detail::AllocationJournalEntry> journal)
    -> std::expected<PageLivenessUpdateDiagnostics, Error> {
  const auto started = std::chrono::steady_clock::now();
  try {
    const auto position =
        std::find_if(index.protected_roots.begin(), index.protected_roots.end(),
                     [&](const PageLivenessRoot& root) -> bool {
                       return root.metadata_slot == overwritten_slot;
                     });
    if (position == index.protected_roots.end())
      return std::unexpected(Error::invalid_metadata);
    PageLivenessUpdateDiagnostics diagnostics;
    auto phase_started = std::chrono::steady_clock::now();
    const auto registered = register_allocations(index, store, journal);
    if (!registered) return std::unexpected(registered.error());
    diagnostics.registration_elapsed =
        std::chrono::steady_clock::now() - phase_started;
    if (candidate_root != invalid_page_id) {
      phase_started = std::chrono::steady_clock::now();
      const auto added =
          add_reference(index, store, candidate_root, candidate_extent,
                        &diagnostics.activated_pages);
      if (!added) return std::unexpected(added.error());
      diagnostics.activation_elapsed =
          std::chrono::steady_clock::now() - phase_started;
    }
    const PageLivenessRoot replaced = *position;
    if (replaced.root != invalid_page_id) {
      phase_started = std::chrono::steady_clock::now();
      const auto removed =
          remove_reference(index, store, replaced.root, replaced.extent,
                           &diagnostics.deactivated_pages);
      if (!removed) return std::unexpected(removed.error());
      diagnostics.deactivation_elapsed =
          std::chrono::steady_clock::now() - phase_started;
    }
    position->root = candidate_root;
    position->extent = candidate_extent;
    position->generation = candidate_generation;
    phase_started = std::chrono::steady_clock::now();
    refresh_reclaimable(index);
    diagnostics.candidate_refresh_elapsed =
        std::chrono::steady_clock::now() - phase_started;
    diagnostics.free_candidates = index.reclaimable.size();
    diagnostics.journaled_pages = journal.size();
    for (const auto& entry : journal) {
      diagnostics.journaled_reclaimable_pages += !index.live(entry.page);
      if (entry.source == storage_detail::AllocationSource::reused)
        ++diagnostics.reused_pages;
      else
        ++diagnostics.appended_pages;
    }
    diagnostics.elapsed = std::chrono::steady_clock::now() - started;
    return diagnostics;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::out_of_memory);
  }
}

}  // namespace tavern
