#include "reachability.hpp"

#include <algorithm>
#include <unordered_map>

#include "adjacency_tree.hpp"
namespace tavern {
namespace {
std::size_t reachability_scan_count = 0;
struct AdjacencyMemoKey {
  PageId root = invalid_page_id;
  std::uint32_t height = 0;
  std::uint64_t count = 0;

  auto operator==(const AdjacencyMemoKey&) const -> bool = default;
};

struct AdjacencyMemoKeyHash {
  auto operator()(const AdjacencyMemoKey& key) const -> std::size_t {
    const std::uint64_t mixed =
        key.root ^ (static_cast<std::uint64_t>(key.height) << 32U) ^ key.count;
    return static_cast<std::size_t>(mixed ^ (mixed >> 33U));
  }
};

struct AdjacencyMemoValue {
  PageId maximum_page = invalid_page_id;
  PageId validated_extent = first_data_page;
  std::size_t page_visits = 0;
};

auto scan_reachable_pages_impl(const PageStore& store,
                               ReachabilityScanDiagnostics* diagnostics)
    -> std::expected<ReachabilityScan, Error> {
  if (store.file_descriptor < 0 || store.page_count < first_data_page)
    return std::unexpected(Error::invalid_page);
  try {
    ReachabilityScan result{};
    result.physical_data_pages = store.page_count - first_data_page;
    const auto bitmap_start = diagnostics
                                  ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{};
    result.reachable.resize(static_cast<std::size_t>(store.page_count), false);
    if (diagnostics) {
      diagnostics->bitmap_initialization +=
          std::chrono::steady_clock::now() - bitmap_start;
    }
    ProtectedDirectoryScanDiagnostics protected_diagnostics;
    // Reclamation treats both valid slots as live roots. This is intentionally
    // more conservative than the read path: an interrupted metadata write can
    // make either slot the recoverable generation after restart.
    const auto protected_scan = scan_protected_directories(
        store, diagnostics ? &protected_diagnostics : nullptr);
    if (!protected_scan) return std::unexpected(protected_scan.error());
    if (diagnostics) {
      diagnostics->metadata_inspection +=
          protected_diagnostics.metadata_inspection;
      diagnostics->directory_traversal +=
          protected_diagnostics.directory_traversal;
      diagnostics->metadata_slots_inspected +=
          protected_diagnostics.metadata_slots_inspected;
      diagnostics->directory_page_visits +=
          protected_diagnostics.directory_page_visits;
      diagnostics->directory_unique_pages +=
          protected_diagnostics.directory_unique_pages;
      diagnostics->repeated_page_visits +=
          protected_diagnostics.directory_page_visits -
          protected_diagnostics.directory_unique_pages;
    }
    const auto mark = [&](PageId id, PageType) -> std::expected<void, Error> {
      if (id < first_data_page || id >= store.page_count)
        return std::unexpected(Error::invalid_page);
      const auto marking_start = diagnostics
                                     ? std::chrono::steady_clock::now()
                                     : std::chrono::steady_clock::time_point{};
      if (diagnostics && result.reachable[static_cast<std::size_t>(id)])
        ++diagnostics->repeated_page_visits;
      result.reachable[static_cast<std::size_t>(id)] = true;
      if (diagnostics)
        diagnostics->bitmap_marking +=
            std::chrono::steady_clock::now() - marking_start;
      return {};
    };
    for (const PageId id : protected_scan->directory_pages) {
      const auto marked = mark(id, PageType::directory_node);
      if (!marked) return std::unexpected(marked.error());
    }
    // The same immutable adjacency descriptor can occur in records from both
    // generations. A scan-local memo avoids revisiting it only when its already
    // observed maximum PageId fits the next generation's extent; it never makes
    // a cross-scan liveness claim.
    std::unordered_map<AdjacencyMemoKey, AdjacencyMemoValue,
                       AdjacencyMemoKeyHash>
        adjacency_memo;
    for (const auto& adjacency : protected_scan->adjacency_descriptors) {
      if (adjacency.descriptor.count == 0) {
        const auto traversed = traverse_adjacency_pages(
            store, adjacency.descriptor, adjacency.committed_extent, mark);
        if (!traversed) return std::unexpected(traversed.error());
        continue;
      }
      const AdjacencyMemoKey key{adjacency.descriptor.root,
                                 adjacency.descriptor.height,
                                 adjacency.descriptor.count};
      const auto memo = adjacency_memo.find(key);
      if (memo != adjacency_memo.end() &&
          memo->second.maximum_page < adjacency.committed_extent) {
        if (diagnostics) {
          ++diagnostics->adjacency_memo_hits;
          if (memo->second.validated_extent != adjacency.committed_extent)
            ++diagnostics->adjacency_memo_extent_relaxed_hits;
          diagnostics->adjacency_memo_skipped_page_visits +=
              memo->second.page_visits;
        }
        continue;
      }
      if (diagnostics) ++diagnostics->adjacency_memo_misses;
      const auto adjacency_start =
          diagnostics ? std::chrono::steady_clock::now()
                      : std::chrono::steady_clock::time_point{};
      std::chrono::nanoseconds adjacency_marking{};
      PageId maximum_page = first_data_page;
      std::size_t page_visits = 0;
      const auto traversed = traverse_adjacency_pages(
          store, adjacency.descriptor, adjacency.committed_extent,
          [&](PageId id, PageType type) -> std::expected<void, Error> {
            if (diagnostics) {
              ++diagnostics->adjacency_page_visits;
              if (!result.reachable[static_cast<std::size_t>(id)])
                ++diagnostics->adjacency_unique_pages;
            }
            maximum_page = std::max(maximum_page, id);
            ++page_visits;
            const auto marking_start =
                diagnostics ? std::chrono::steady_clock::now()
                            : std::chrono::steady_clock::time_point{};
            const auto marked = mark(id, type);
            if (diagnostics)
              adjacency_marking +=
                  std::chrono::steady_clock::now() - marking_start;
            return marked;
          });
      if (diagnostics) {
        const auto elapsed = std::chrono::steady_clock::now() - adjacency_start;
        diagnostics->adjacency_traversal += elapsed - adjacency_marking;
      }
      if (!traversed) return std::unexpected(traversed.error());
      adjacency_memo.insert_or_assign(
          key, AdjacencyMemoValue{maximum_page, adjacency.committed_extent,
                                  page_visits});
    }
    const auto counting_start = diagnostics
                                    ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
    for (PageId id = first_data_page; id < store.page_count; ++id)
      if (result.reachable[static_cast<std::size_t>(id)])
        ++result.reachable_data_pages;
    if (diagnostics) {
      diagnostics->bitmap_counting +=
          std::chrono::steady_clock::now() - counting_start;
    }
    result.unreachable_data_pages =
        result.physical_data_pages - result.reachable_data_pages;
    if (diagnostics) {
      diagnostics->physical_data_pages = result.physical_data_pages;
      diagnostics->reachable_data_pages = result.reachable_data_pages;
      diagnostics->unreachable_data_pages = result.unreachable_data_pages;
      diagnostics->logical_page_reads = diagnostics->metadata_slots_inspected +
                                        diagnostics->directory_page_visits +
                                        diagnostics->adjacency_page_visits;
      diagnostics->adjacency_memo_entries = adjacency_memo.size();
      diagnostics->adjacency_memo_payload_bytes =
          adjacency_memo.size() * sizeof(AdjacencyMemoValue);
    }
    return result;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::out_of_memory);
  }
}
}  // namespace

auto scan_reachable_pages(const PageStore& store)
    -> std::expected<ReachabilityScan, Error> {
  ++reachability_scan_count;
  return scan_reachable_pages_impl(store, nullptr);
}

auto profile_reachable_pages(const PageStore& store)
    -> std::expected<ProfiledReachabilityScan, Error> {
  ++reachability_scan_count;
  ReachabilityScanDiagnostics diagnostics;
  const auto start = std::chrono::steady_clock::now();
  auto scan = scan_reachable_pages_impl(store, &diagnostics);
  diagnostics.total = std::chrono::steady_clock::now() - start;
  if (!scan) return std::unexpected(scan.error());
  return ProfiledReachabilityScan{std::move(*scan), diagnostics};
}
void reset_reachability_scan_count_for_test() { reachability_scan_count = 0; }
auto reachability_scan_count_for_test() -> std::size_t {
  return reachability_scan_count;
}
}  // namespace tavern
