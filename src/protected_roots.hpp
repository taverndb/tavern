#pragma once
#include <chrono>
#include <unordered_set>

#include "directory.hpp"
namespace tavern {
// These are the roots that reclamation must preserve. Either metadata slot may
// be selected after a crash, so a page is reclaimable only if no valid slot can
// reach it. Each extent is carried with its root to bound traversal to the
// generation that named it.
struct ProtectedRoot {
  PageId root = invalid_page_id;
  std::uint64_t generation = 0;
  PageId extent = first_data_page;
};
struct ProtectedAdjacency {
  AdjacencyDescriptor descriptor;
  PageId committed_extent = first_data_page;
};
struct ProtectedDirectoryScan {
  std::unordered_set<PageId> directory_pages;
  std::vector<ProtectedAdjacency> adjacency_descriptors;
  std::vector<ProtectedRoot> roots;
};
struct ProtectedDirectoryScanDiagnostics {
  std::chrono::nanoseconds metadata_inspection{};
  std::chrono::nanoseconds directory_traversal{};
  std::size_t metadata_slots_inspected = 0;
  std::size_t directory_page_visits = 0;
  std::size_t directory_unique_pages = 0;
};
std::expected<ProtectedDirectoryScan, Error> scan_protected_directories(
    const PageStore& store);
std::expected<ProtectedDirectoryScan, Error> scan_protected_directories(
    const PageStore& store, ProtectedDirectoryScanDiagnostics* diagnostics);
}  // namespace tavern
