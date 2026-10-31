#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <vector>

#include "tavern.hpp"

namespace tavern {

// The database is a single fixed-page file. A PageId is therefore both a
// logical address and the page's zero-based physical index; its byte offset is
// PageId * page_size after overflow checking. Slots 0 and 1 are never data:
// they hold alternating commit records. `invalid_page_id` is an in-memory and
// on-disk null pointer, not an address that may be read or written.
using PageId = std::uint64_t;

inline constexpr std::size_t page_size = 4096;
inline constexpr PageId metadata_slot_a = 0;
inline constexpr PageId metadata_slot_b = 1;
inline constexpr PageId first_data_page = 2;
inline constexpr PageId invalid_page_id = UINT64_MAX;

enum class PageType : std::uint16_t {
  data = 1,
  metadata = 2,
  directory_node = 3,
  directory_leaf = 4,
  adjacency_leaf = 5,
  adjacency_reference = 6,
};

// A decoded page. The on-disk representation is always exactly `page_size`
// bytes: a 20-byte little-endian header (magic, format, type, payload length,
// checksum) followed by payload bytes and zero-filled unused space. The
// checksum covers the complete payload area, including padding, so a torn or
// modified page is rejected before its typed payload is interpreted.
struct Page {
  PageType type = PageType::data;
  std::vector<std::byte> payload;
};

// The durable publication record. `committed_page_count` is the generation's
// logical extent: readers must not follow references at or beyond it even when
// the physical file is longer because later, unpublished writes were appended.
struct CommitMetadata {
  std::uint64_t generation = 0;
  PageId root = invalid_page_id;
  PageId committed_page_count = first_data_page;
};

// Owns the exclusively locked database descriptor. It is movable but not
// copyable so that exactly one PageStore closes the descriptor. Callers use
// positional I/O, hence reads and writes do not share a seek position; the
// store itself nevertheless has no general concurrent-operation protocol.
// Database operations are expected to serialize access while it is open.
//
// `page_count` describes the physical, page-aligned file length, whereas a
// CommitMetadata extent describes the currently published generation. The
// allocation-session fields temporarily redirect structural writes through a
// reservation owner; they prevent accidental ordinary appends during reuse.
struct PageStore {
  using EncodedPageAllocator =
      std::expected<PageId, Error> (*)(void*, std::span<const std::byte>);
  int file_descriptor = -1;
  PageId page_count = 0;
  bool fail_metadata_write_for_test = false;
  bool fail_metadata_sync_for_test = false;
  bool fail_data_sync_for_test = false;
  std::int64_t fail_append_after_for_test = -1;
  std::int64_t fail_reusable_write_after_for_test = -1;
  bool allocation_session_active = false;
  EncodedPageAllocator session_allocator = nullptr;
  void* session_allocator_context = nullptr;
  mutable bool page_read_tracking_for_test = false;
  mutable std::size_t page_read_operations_for_test = 0;
  mutable std::vector<bool> page_read_pages_for_test;

  PageStore() = default;
  PageStore(int descriptor, PageId count)
      : file_descriptor(descriptor), page_count(count) {}
  ~PageStore();
  PageStore(const PageStore&) = delete;
  PageStore& operator=(const PageStore&) = delete;
  PageStore(PageStore&& other) noexcept;
  PageStore& operator=(PageStore&& other) noexcept;
};

struct PageReadMetrics {
  std::size_t logical_reads = 0;
  std::size_t unique_pages = 0;
};

enum class MetadataWriteOutcome { success, not_published, indeterminate };
enum class CrashPoint {
  none,
  before_data_sync,
  after_data_sync,
  after_metadata_write,
  after_metadata_sync,
  during_reusable_write
};
void set_crash_point_for_test(CrashPoint point);
void crash_if_selected_for_test(CrashPoint point);

std::expected<PageStore, Error> create_page_store(
    const std::filesystem::path& path);
std::expected<PageStore, Error> open_page_store(
    const std::filesystem::path& path);
std::expected<void, Error> close_page_store(PageStore& store);
std::expected<void, Error> sync_page_store(const PageStore& store);
void begin_page_read_tracking_for_test(PageStore& store);
PageReadMetrics end_page_read_tracking_for_test(PageStore& store);

// Appends an immutable data page. Metadata pages are written only through the
// dedicated publication path. A successful write is not durable until sync.
std::expected<PageId, Error> append_page(PageStore& store, PageType type,
                                         std::span<const std::byte> payload);
std::expected<Page, Error> read_page(const PageStore& store, PageId id);
std::expected<CommitMetadata, Error> read_metadata(const PageStore& store,
                                                   PageId slot);
// Replaces one metadata slot and synchronizes it. Once the write attempt has
// begun, an I/O error is deliberately reported as indeterminate: callers must
// reopen and select durable metadata rather than assume the old generation won.
MetadataWriteOutcome write_metadata_durable(PageStore& store, PageId slot,
                                            const CommitMetadata& metadata);

}  // namespace tavern
