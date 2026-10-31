#pragma once
#include <chrono>

#include "protected_roots.hpp"
namespace tavern {
// A scan marks physical data pages reachable from every valid metadata slot,
// not merely the generation currently selected for reads. `reachable` is
// indexed by physical PageId; metadata pages are excluded from its counts.
// Therefore an unmarked page is safe to offer for reuse only while the file and
// both protected metadata roots remain the ones that were scanned.
struct ReachabilityScan {
  PageId physical_data_pages = 0;
  PageId reachable_data_pages = 0;
  PageId unreachable_data_pages = 0;
  std::vector<bool> reachable;
};
struct ReachabilityScanDiagnostics {
  std::chrono::nanoseconds total{};
  std::chrono::nanoseconds bitmap_initialization{};
  std::chrono::nanoseconds metadata_inspection{};
  std::chrono::nanoseconds directory_traversal{};
  std::chrono::nanoseconds adjacency_traversal{};
  std::chrono::nanoseconds bitmap_marking{};
  std::chrono::nanoseconds bitmap_counting{};
  PageId physical_data_pages = 0;
  PageId reachable_data_pages = 0;
  PageId unreachable_data_pages = 0;
  std::size_t metadata_slots_inspected = 0;
  std::size_t directory_page_visits = 0;
  std::size_t directory_unique_pages = 0;
  std::size_t adjacency_page_visits = 0;
  std::size_t adjacency_unique_pages = 0;
  std::size_t adjacency_memo_hits = 0;
  std::size_t adjacency_memo_extent_relaxed_hits = 0;
  std::size_t adjacency_memo_misses = 0;
  std::size_t adjacency_memo_skipped_page_visits = 0;
  std::size_t adjacency_memo_entries = 0;
  std::size_t adjacency_memo_payload_bytes = 0;
  std::size_t repeated_page_visits = 0;
  std::size_t logical_page_reads = 0;
};
struct ProfiledReachabilityScan {
  ReachabilityScan scan;
  ReachabilityScanDiagnostics diagnostics;
};
std::expected<ReachabilityScan, Error> scan_reachable_pages(
    const PageStore& store);
std::expected<ProfiledReachabilityScan, Error> profile_reachable_pages(
    const PageStore& store);
void reset_reachability_scan_count_for_test();
std::size_t reachability_scan_count_for_test();
}  // namespace tavern
