#include "protected_roots.hpp"
namespace tavern {
namespace {
auto valid_descriptor(const AdjacencyDescriptor& d, PageId extent) -> bool {
  return d.count == 0
             ? (d.root == invalid_page_id && d.height == 0)
             : (d.root != invalid_page_id && d.root < extent && d.count > 0);
}
}  // namespace
auto scan_protected_directories(const PageStore& store)
    -> std::expected<ProtectedDirectoryScan, Error> {
  return scan_protected_directories(store, nullptr);
}

auto scan_protected_directories(const PageStore& store,
                                ProtectedDirectoryScanDiagnostics* diagnostics)
    -> std::expected<ProtectedDirectoryScan, Error> {
  if (store.file_descriptor < 0) return std::unexpected(Error::io_failure);
  ProtectedDirectoryScan result;
  for (const PageId slot : {metadata_slot_a, metadata_slot_b}) {
    const auto metadata_start = diagnostics
                                    ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
    const auto metadata = read_metadata(store, slot);
    if (diagnostics) {
      diagnostics->metadata_inspection +=
          std::chrono::steady_clock::now() - metadata_start;
      ++diagnostics->metadata_slots_inspected;
    }
    if (!metadata) continue;
    if (metadata->committed_page_count > store.page_count ||
        metadata->committed_page_count < first_data_page ||
        (metadata->root != invalid_page_id &&
         metadata->root >= metadata->committed_page_count))
      return std::unexpected(Error::invalid_metadata);
    bool duplicate = false;
    for (const auto& root : result.roots)
      duplicate = duplicate || root.root == metadata->root;
    if (!duplicate)
      result.roots.push_back({metadata->root, metadata->generation,
                              metadata->committed_page_count});
    const auto directory_start = diagnostics
                                     ? std::chrono::steady_clock::now()
                                     : std::chrono::steady_clock::time_point{};
    const auto traversed = traverse_directory(
        store, metadata->root, metadata->committed_page_count,
        [&](PageId id, PageType) -> std::expected<void, Error> {
          if (diagnostics) ++diagnostics->directory_page_visits;
          result.directory_pages.insert(id);
          return std::expected<void, Error>{};
        },
        [&](const PatronRecord& record) -> std::expected<void, Error> {
          for (const auto& d :
               {record.following, record.followers, record.friends}) {
            if (!valid_descriptor(d, metadata->committed_page_count))
              return std::expected<void, Error>(
                  std::unexpected(Error::invalid_directory));
            result.adjacency_descriptors.push_back(
                {d, metadata->committed_page_count});
          }
          return std::expected<void, Error>{};
        });
    if (diagnostics) {
      diagnostics->directory_traversal +=
          std::chrono::steady_clock::now() - directory_start;
    }
    if (!traversed) return std::unexpected(traversed.error());
  }
  if (result.roots.empty()) return std::unexpected(Error::invalid_metadata);
  if (diagnostics)
    diagnostics->directory_unique_pages = result.directory_pages.size();
  return result;
}
}  // namespace tavern
