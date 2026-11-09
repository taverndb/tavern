#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "protected_roots.hpp"

namespace tavern {
namespace storage_detail {
struct AllocationJournalEntry;
}

struct PageLivenessRoot {
  PageId metadata_slot = invalid_page_id;
  PageId root = invalid_page_id;
  PageId extent = first_data_page;
  std::uint64_t generation = 0;
};

struct PageLivenessIndex {
  PageId physical_page_count = 0;
  std::vector<std::uint32_t> incoming_references;
  std::vector<PageLivenessRoot> protected_roots;
  std::vector<PageId> reclaimable;
  // Non-production reference-count model used by test support and benchmarks.
  // The retained FreePagePool is the production reclamation mechanism.
  bool valid = true;

  bool live(PageId page) const;
  std::size_t memory_bytes() const;
};

struct PageLivenessUpdateDiagnostics {
  std::chrono::nanoseconds elapsed{};
  std::chrono::nanoseconds registration_elapsed{};
  std::chrono::nanoseconds activation_elapsed{};
  std::chrono::nanoseconds deactivation_elapsed{};
  std::chrono::nanoseconds candidate_refresh_elapsed{};
  std::size_t activated_pages = 0;
  std::size_t deactivated_pages = 0;
  std::size_t free_candidates = 0;
  std::size_t journaled_pages = 0;
  std::size_t journaled_reclaimable_pages = 0;
  std::size_t reused_pages = 0;
  std::size_t appended_pages = 0;
};

std::expected<PageLivenessIndex, Error> build_page_liveness_index(
    const PageStore& store);
std::expected<void, Error> simulate_page_liveness_root_replacement(
    PageLivenessIndex& index, const PageStore& store, PageId overwritten_slot,
    PageId candidate_root, PageId candidate_extent);
std::expected<PageLivenessUpdateDiagnostics, Error> update_page_liveness_index(
    PageLivenessIndex& index, const PageStore& store, PageId overwritten_slot,
    PageId candidate_root, PageId candidate_extent,
    std::uint64_t candidate_generation,
    std::span<const storage_detail::AllocationJournalEntry> journal);

}  // namespace tavern
