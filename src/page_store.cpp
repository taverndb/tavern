#include "page_store.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <new>

namespace tavern {

namespace {
CrashPoint crash_point_for_test = CrashPoint::none;
}
void set_crash_point_for_test(CrashPoint point) {
  crash_point_for_test = point;
}
void crash_if_selected_for_test(CrashPoint point) {
  if (crash_point_for_test == point) {
    _exit(99);
  }
}

PageStore::~PageStore() {
  if (file_descriptor >= 0) {
    (void)::close(file_descriptor);
  }
}

PageStore::PageStore(PageStore&& other) noexcept
    : file_descriptor(other.file_descriptor),
      page_count(other.page_count),
      fail_metadata_write_for_test(other.fail_metadata_write_for_test),
      fail_metadata_sync_for_test(other.fail_metadata_sync_for_test),
      fail_data_sync_for_test(other.fail_data_sync_for_test),
      fail_append_after_for_test(other.fail_append_after_for_test),
      fail_reusable_write_after_for_test(
          other.fail_reusable_write_after_for_test),
      allocation_session_active(other.allocation_session_active),
      session_allocator(other.session_allocator),
      session_allocator_context(other.session_allocator_context),
      page_read_tracking_for_test(other.page_read_tracking_for_test),
      page_read_operations_for_test(other.page_read_operations_for_test),
      page_read_pages_for_test(std::move(other.page_read_pages_for_test)) {
  other.file_descriptor = -1;
}

auto PageStore::operator=(PageStore&& other) noexcept -> PageStore& {
  if (this != &other) {
    if (file_descriptor >= 0) {
      (void)::close(file_descriptor);
    }
    file_descriptor = other.file_descriptor;
    page_count = other.page_count;
    fail_metadata_write_for_test = other.fail_metadata_write_for_test;
    fail_metadata_sync_for_test = other.fail_metadata_sync_for_test;
    fail_data_sync_for_test = other.fail_data_sync_for_test;
    fail_append_after_for_test = other.fail_append_after_for_test;
    fail_reusable_write_after_for_test =
        other.fail_reusable_write_after_for_test;
    allocation_session_active = other.allocation_session_active;
    session_allocator = other.session_allocator;
    session_allocator_context = other.session_allocator_context;
    page_read_tracking_for_test = other.page_read_tracking_for_test;
    page_read_operations_for_test = other.page_read_operations_for_test;
    page_read_pages_for_test = std::move(other.page_read_pages_for_test);
    other.file_descriptor = -1;
  }
  return *this;
}

namespace {

auto lock_store(int descriptor) -> std::expected<void, Error> {
  if (flock(descriptor, LOCK_EX | LOCK_NB) == 0) return {};
  return std::unexpected((errno == EWOULDBLOCK || errno == EAGAIN)
                             ? Error::database_locked
                             : Error::io_failure);
}

constexpr std::uint32_t page_magic =
    0x47505654U;  // "TVPG" in little-endian bytes.
constexpr std::uint32_t metadata_magic =
    0x444D5654U;  // "TVMD" in little-endian bytes.
constexpr std::uint16_t format_version = 2;
constexpr std::size_t page_header_size = 20;
constexpr std::size_t metadata_payload_size = 40;

// RawPage is deliberately a whole-page buffer. Encoding zeros it first, which
// makes checksum coverage deterministic and means a short payload cannot retain
// bytes from an earlier use of an in-memory buffer.
using RawPage = std::array<std::byte, page_size>;

void write_u16(std::span<std::byte> bytes, std::size_t offset,
               std::uint16_t value) {
  bytes[offset] = static_cast<std::byte>(value & 0xFFU);
  bytes[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
}

void write_u32(std::span<std::byte> bytes, std::size_t offset,
               std::uint32_t value) {
  for (std::size_t index = 0; index < 4; ++index) {
    bytes[offset + index] =
        static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
  }
}

void write_u64(std::span<std::byte> bytes, std::size_t offset,
               std::uint64_t value) {
  for (std::size_t index = 0; index < 8; ++index) {
    bytes[offset + index] =
        static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
  }
}

auto read_u16(std::span<const std::byte> bytes, std::size_t offset)
    -> std::uint16_t {
  return static_cast<std::uint16_t>(
             std::to_integer<unsigned int>(bytes[offset])) |
         static_cast<std::uint16_t>(
             std::to_integer<unsigned int>(bytes[offset + 1]) << 8U);
}

auto read_u32(std::span<const std::byte> bytes, std::size_t offset)
    -> std::uint32_t {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(
                 std::to_integer<unsigned int>(bytes[offset + index]))
             << (index * 8U);
  }
  return value;
}

auto read_u64(std::span<const std::byte> bytes, std::size_t offset)
    -> std::uint64_t {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(
                 std::to_integer<unsigned long long>(bytes[offset + index]))
             << (index * 8U);
  }
  return value;
}

auto checksum(std::span<const std::byte> bytes) -> std::uint64_t {
  std::uint64_t value = 14695981039346656037ULL;
  for (const std::byte byte : bytes) {
    value ^= static_cast<std::uint64_t>(std::to_integer<unsigned int>(byte));
    value *= 1099511628211ULL;
  }
  return value;
}

auto page_offset(PageId id, off_t& result) -> bool {
  constexpr auto max_offset =
      static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
  if (id > max_offset / page_size) {
    return false;
  }
  result = static_cast<off_t>(id * page_size);
  return true;
}

auto write_exact(int file_descriptor, std::span<const std::byte> bytes,
                 off_t offset) -> std::expected<void, Error> {
  std::size_t completed = 0;
  // pwrite/pread may complete only a prefix and may be interrupted. Retrying
  // establishes complete *kernel* I/O, not durability; fsync is a separate
  // publication-order step.
  while (completed < bytes.size()) {
    const ssize_t written = pwrite(file_descriptor, bytes.data() + completed,
                                   bytes.size() - completed,
                                   offset + static_cast<off_t>(completed));
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return std::unexpected(Error::io_failure);
    }
    if (written == 0) {
      return std::unexpected(Error::io_failure);
    }
    completed += static_cast<std::size_t>(written);
  }
  return {};
}

auto write_reusable_exact(PageStore& store, std::span<const std::byte> bytes,
                          off_t offset) -> std::expected<void, Error> {
  std::size_t completed = 0;
  while (completed < bytes.size()) {
    if (store.fail_reusable_write_after_for_test == 0) {
      store.fail_reusable_write_after_for_test = -1;
      return std::unexpected(Error::io_failure);
    }
    std::size_t requested = bytes.size() - completed;
    if (store.fail_reusable_write_after_for_test > 0) {
      const auto limit =
          static_cast<std::size_t>(store.fail_reusable_write_after_for_test);
      requested = std::min(requested, limit);
    }
    const ssize_t written =
        pwrite(store.file_descriptor, bytes.data() + completed, requested,
               offset + static_cast<off_t>(completed));
    if (written < 0) {
      if (errno == EINTR) continue;
      return std::unexpected(Error::io_failure);
    }
    if (written == 0) return std::unexpected(Error::io_failure);
    completed += static_cast<std::size_t>(written);
    crash_if_selected_for_test(CrashPoint::during_reusable_write);
    if (store.fail_reusable_write_after_for_test > 0)
      store.fail_reusable_write_after_for_test -= written;
  }
  return {};
}

auto encode_page(PageType type, std::span<const std::byte> payload)
    -> std::expected<RawPage, Error>;

auto append_page_impl(PageStore& store, PageType type,
                      std::span<const std::byte> payload, bool session_append)
    -> std::expected<PageId, Error> {
  if (store.file_descriptor < 0 || store.page_count == invalid_page_id ||
      (store.allocation_session_active && !session_append)) {
    return std::unexpected(Error::page_limit);
  }
  const auto page = encode_page(type, payload);
  if (!page) return std::unexpected(page.error());
  if (store.fail_append_after_for_test == 0) {
    store.fail_append_after_for_test = -1;
    return std::unexpected(Error::io_failure);
  }
  if (store.fail_append_after_for_test > 0) --store.fail_append_after_for_test;
  off_t offset = 0;
  if (!page_offset(store.page_count, offset))
    return std::unexpected(Error::page_limit);
  const PageId id = store.page_count;
  const auto result = write_exact(store.file_descriptor, *page, offset);
  if (!result) return std::unexpected(result.error());
  ++store.page_count;
  return id;
}

auto read_exact(int file_descriptor, std::span<std::byte> bytes, off_t offset)
    -> std::expected<void, Error> {
  std::size_t completed = 0;
  while (completed < bytes.size()) {
    const ssize_t read_count =
        pread(file_descriptor, bytes.data() + completed,
              bytes.size() - completed, offset + static_cast<off_t>(completed));
    if (read_count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return std::unexpected(Error::io_failure);
    }
    if (read_count == 0) {
      return std::unexpected(Error::io_failure);
    }
    completed += static_cast<std::size_t>(read_count);
  }
  return {};
}

auto encode_page(PageType type, std::span<const std::byte> payload)
    -> std::expected<RawPage, Error> {
  if (payload.size() > page_size - page_header_size) {
    return std::unexpected(Error::invalid_page);
  }
  RawPage page{};
  std::span<std::byte> bytes(page);
  write_u32(bytes, 0, page_magic);
  write_u16(bytes, 4, format_version);
  write_u16(bytes, 6, static_cast<std::uint16_t>(type));
  write_u32(bytes, 8, static_cast<std::uint32_t>(payload.size()));
  std::copy(payload.begin(), payload.end(),
            page.begin() + static_cast<std::ptrdiff_t>(page_header_size));
  write_u64(
      bytes, 12,
      checksum(std::span<const std::byte>(page).subspan(page_header_size)));
  return page;
}

auto decode_page(const RawPage& raw) -> std::expected<Page, Error> {
  const std::span<const std::byte> bytes(raw);
  if (read_u32(bytes, 0) != page_magic ||
      read_u16(bytes, 4) != format_version) {
    return std::unexpected(Error::invalid_page);
  }
  const std::uint16_t raw_type = read_u16(bytes, 6);
  if (raw_type != static_cast<std::uint16_t>(PageType::data) &&
      raw_type != static_cast<std::uint16_t>(PageType::metadata) &&
      raw_type != static_cast<std::uint16_t>(PageType::directory_node) &&
      raw_type != static_cast<std::uint16_t>(PageType::directory_leaf) &&
      raw_type != static_cast<std::uint16_t>(PageType::adjacency_leaf) &&
      raw_type != static_cast<std::uint16_t>(PageType::adjacency_reference)) {
    return std::unexpected(Error::invalid_page);
  }
  const std::uint32_t payload_size = read_u32(bytes, 8);
  if (payload_size > page_size - page_header_size ||
      read_u64(bytes, 12) != checksum(bytes.subspan(page_header_size))) {
    return std::unexpected(Error::invalid_page);
  }
  try {
    return Page{
        static_cast<PageType>(raw_type),
        std::vector<std::byte>(
            bytes.begin() + static_cast<std::ptrdiff_t>(page_header_size),
            bytes.begin() +
                static_cast<std::ptrdiff_t>(page_header_size + payload_size))};
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::out_of_memory);
  }
}

auto read_raw_page(const PageStore& store, PageId id)
    -> std::expected<RawPage, Error> {
  if (store.file_descriptor < 0 || id >= store.page_count) {
    return std::unexpected(Error::invalid_page);
  }
  off_t offset = 0;
  if (!page_offset(id, offset)) {
    return std::unexpected(Error::invalid_page);
  }
  RawPage page{};
  const auto result = read_exact(store.file_descriptor, page, offset);
  if (!result) {
    return std::unexpected(result.error());
  }
  return page;
}

auto initialize_metadata(PageStore& store, PageId slot)
    -> std::expected<void, Error> {
  std::array<std::byte, metadata_payload_size> payload{};
  std::span<std::byte> bytes(payload);
  write_u32(bytes, 0, metadata_magic);
  write_u16(bytes, 4, format_version);
  write_u64(bytes, 8, 0);
  write_u64(bytes, 16, invalid_page_id);
  write_u64(bytes, 24, first_data_page);
  write_u64(bytes, 32, checksum(std::span<const std::byte>(payload).first(32)));
  const auto page = encode_page(PageType::metadata, payload);
  if (!page) {
    return std::unexpected(page.error());
  }
  off_t offset = 0;
  if (!page_offset(slot, offset)) {
    return std::unexpected(Error::invalid_page);
  }
  return write_exact(store.file_descriptor, *page, offset);
}

auto write_metadata_durable_impl(PageStore& store, PageId slot,
                                 const CommitMetadata& metadata)
    -> MetadataWriteOutcome {
  if ((slot != metadata_slot_a && slot != metadata_slot_b) ||
      store.file_descriptor < 0 ||
      metadata.committed_page_count < first_data_page ||
      metadata.committed_page_count > store.page_count ||
      (metadata.root != invalid_page_id &&
       metadata.root >= metadata.committed_page_count)) {
    return MetadataWriteOutcome::not_published;
  }
  std::array<std::byte, metadata_payload_size> payload{};
  std::span<std::byte> bytes(payload);
  write_u32(bytes, 0, metadata_magic);
  write_u16(bytes, 4, format_version);
  write_u64(bytes, 8, metadata.generation);
  write_u64(bytes, 16, metadata.root);
  write_u64(bytes, 24, metadata.committed_page_count);
  write_u64(bytes, 32, checksum(std::span<const std::byte>(payload).first(32)));
  const auto page = encode_page(PageType::metadata, payload);
  if (!page) return MetadataWriteOutcome::not_published;
  off_t offset = 0;
  if (!page_offset(slot, offset)) return MetadataWriteOutcome::not_published;
  // A metadata overwrite is the only in-place write to a protected page. Its
  // inactive-slot role ensures the other slot still names a prior generation
  // until this replacement has reached stable storage. After attempting it,
  // failures are indeterminate: the device may persist some or all of the
  // write despite the syscall result.
  const bool write_attempted = true;
  if (store.fail_metadata_write_for_test)
    return MetadataWriteOutcome::indeterminate;
  const auto written = write_exact(store.file_descriptor, *page, offset);
  if (!written)
    return write_attempted ? MetadataWriteOutcome::indeterminate
                           : MetadataWriteOutcome::not_published;
  crash_if_selected_for_test(CrashPoint::after_metadata_write);
  if (store.fail_metadata_sync_for_test || fsync(store.file_descriptor) != 0)
    return MetadataWriteOutcome::indeterminate;
  crash_if_selected_for_test(CrashPoint::after_metadata_sync);
  return MetadataWriteOutcome::success;
}

auto sync_parent_directory(const std::filesystem::path& path)
    -> std::expected<void, Error> {
  const std::filesystem::path parent =
      path.has_parent_path() ? path.parent_path() : ".";
  const int descriptor = ::open(parent.c_str(), O_RDONLY);
  if (descriptor < 0) {
    return std::unexpected(Error::io_failure);
  }
  const bool synchronized = fsync(descriptor) == 0;
  const bool closed = ::close(descriptor) == 0;
  return synchronized && closed
             ? std::expected<void, Error>{}
             : std::expected<void, Error>(std::unexpected(Error::io_failure));
}

}  // namespace

auto write_metadata_durable(PageStore& store, PageId slot,
                            const CommitMetadata& metadata)
    -> MetadataWriteOutcome {
  if (store.allocation_session_active)
    return MetadataWriteOutcome::not_published;
  return write_metadata_durable_impl(store, slot, metadata);
}

auto create_page_store(const std::filesystem::path& path)
    -> std::expected<PageStore, Error> {
  const int descriptor =
      ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (descriptor < 0) {
    return std::unexpected(Error::io_failure);
  }
  // An advisory non-blocking exclusive lock makes one open PageStore the sole
  // cooperating writer/reader. Filesystems or external programs that ignore
  // flock are outside this protection model.
  const auto locked = lock_store(descriptor);
  if (!locked) {
    (void)::close(descriptor);
    return std::unexpected(locked.error());
  }
  PageStore store{descriptor, first_data_page};
  const auto first = initialize_metadata(store, metadata_slot_a);
  const auto second =
      first ? initialize_metadata(store, metadata_slot_b)
            : std::expected<void, Error>(std::unexpected(first.error()));
  if (!first || !second || fsync(descriptor) != 0) {
    (void)::close(descriptor);
    return std::unexpected(
        !first ? first.error()
               : (!second ? second.error() : Error::io_failure));
  }
  // Synchronizing the parent makes successful creation durable as a directory
  // entry as well as a file's contents; later commits need only synchronize the
  // already-existing file.
  const auto directory_result = sync_parent_directory(path);
  if (!directory_result) {
    (void)::close(descriptor);
    return std::unexpected(directory_result.error());
  }
  return store;
}

auto open_page_store(const std::filesystem::path& path)
    -> std::expected<PageStore, Error> {
  const int descriptor = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (descriptor < 0) {
    return std::unexpected(Error::io_failure);
  }
  const auto locked = lock_store(descriptor);
  if (!locked) {
    (void)::close(descriptor);
    return std::unexpected(locked.error());
  }
  struct stat status{};
  if (fstat(descriptor, &status) != 0 ||
      status.st_size < static_cast<off_t>(first_data_page * page_size) ||
      status.st_size % static_cast<off_t>(page_size) != 0) {
    (void)::close(descriptor);
    return std::unexpected(Error::invalid_metadata);
  }
  return PageStore{
      descriptor,
      static_cast<PageId>(status.st_size / static_cast<off_t>(page_size))};
}

auto close_page_store(PageStore& store) -> std::expected<void, Error> {
  if (store.file_descriptor < 0) {
    return {};
  }
  if (::close(store.file_descriptor) != 0) {
    return std::unexpected(Error::io_failure);
  }
  store.file_descriptor = -1;
  return {};
}

auto sync_page_store(const PageStore& store) -> std::expected<void, Error> {
  if (store.fail_data_sync_for_test) return std::unexpected(Error::io_failure);
  return store.file_descriptor >= 0 && fsync(store.file_descriptor) == 0
             ? std::expected<void, Error>{}
             : std::expected<void, Error>(std::unexpected(Error::io_failure));
}

void begin_page_read_tracking_for_test(PageStore& store) {
  store.page_read_tracking_for_test = true;
  store.page_read_operations_for_test = 0;
  store.page_read_pages_for_test.assign(
      static_cast<std::size_t>(store.page_count), false);
}

auto end_page_read_tracking_for_test(PageStore& store) -> PageReadMetrics {
  PageReadMetrics metrics{store.page_read_operations_for_test, 0};
  for (const bool read : store.page_read_pages_for_test)
    metrics.unique_pages += read ? 1U : 0U;
  store.page_read_tracking_for_test = false;
  store.page_read_pages_for_test.clear();
  return metrics;
}

auto append_page(PageStore& store, PageType type,
                 std::span<const std::byte> payload)
    -> std::expected<PageId, Error> {
  return append_page_impl(store, type, payload, false);
}

auto read_page(const PageStore& store, PageId id)
    -> std::expected<Page, Error> {
  if (store.page_read_tracking_for_test &&
      id < store.page_read_pages_for_test.size()) {
    ++store.page_read_operations_for_test;
    store.page_read_pages_for_test[static_cast<std::size_t>(id)] = true;
  }
  const auto raw = read_raw_page(store, id);
  return raw ? decode_page(*raw)
             : std::expected<Page, Error>(std::unexpected(raw.error()));
}

namespace storage_detail {

auto overwrite_reserved_unreachable_page(
    PageStore& store, PageId destination,
    std::span<const std::byte> encoded_page) -> std::expected<void, Error> {
  // Reuse is the sole intentional overwrite of a data page. The caller's
  // reservation proof is essential: overwriting any page reachable from either
  // metadata generation would corrupt the generation that recovery can choose.
  // This low-level function checks shape and encoding, but cannot itself prove
  // reachability; that proof belongs to FreePagePool construction.
  if (store.file_descriptor < 0 || destination < first_data_page ||
      destination >= store.page_count || encoded_page.size() != page_size) {
    return std::unexpected(Error::invalid_page);
  }
  RawPage raw{};
  std::copy(encoded_page.begin(), encoded_page.end(), raw.begin());
  const auto decoded = decode_page(raw);
  if (!decoded || decoded->type == PageType::metadata)
    return std::unexpected(Error::invalid_page);
  off_t offset = 0;
  if (!page_offset(destination, offset))
    return std::unexpected(Error::invalid_page);
  return write_reusable_exact(store, encoded_page, offset);
}

auto append_encoded_page(PageStore& store,
                         std::span<const std::byte> encoded_page)
    -> std::expected<PageId, Error> {
  if (store.file_descriptor < 0 || encoded_page.size() != page_size)
    return std::unexpected(Error::invalid_page);
  RawPage raw{};
  std::copy(encoded_page.begin(), encoded_page.end(), raw.begin());
  const auto decoded = decode_page(raw);
  if (!decoded || decoded->type == PageType::metadata)
    return std::unexpected(Error::invalid_page);
  return append_page_impl(store, decoded->type, decoded->payload, true);
}

auto allocate_page_for_active_session(PageStore& store, PageType type,
                                      std::span<const std::byte> payload)
    -> std::expected<PageId, Error> {
  if (!store.allocation_session_active)
    return append_page(store, type, payload);
  if (!store.session_allocator || !store.session_allocator_context)
    return std::unexpected(Error::invalid_page);
  const auto encoded = encode_page(type, payload);
  if (!encoded) return std::unexpected(encoded.error());
  return store.session_allocator(store.session_allocator_context, *encoded);
}

}  // namespace storage_detail

auto read_metadata(const PageStore& store, PageId slot)
    -> std::expected<CommitMetadata, Error> {
  if (slot != metadata_slot_a && slot != metadata_slot_b) {
    return std::unexpected(Error::invalid_metadata);
  }
  const auto page = read_page(store, slot);
  if (!page || page->type != PageType::metadata ||
      page->payload.size() != metadata_payload_size) {
    return std::unexpected(Error::invalid_metadata);
  }
  const std::span<const std::byte> bytes(page->payload);
  if (read_u32(bytes, 0) != metadata_magic ||
      read_u16(bytes, 4) != format_version ||
      read_u64(bytes, 32) != checksum(bytes.first(32))) {
    return std::unexpected(Error::invalid_metadata);
  }
  const PageId extent = read_u64(bytes, 24);
  if (extent < first_data_page) {
    return std::unexpected(Error::invalid_metadata);
  }
  return CommitMetadata{read_u64(bytes, 8), read_u64(bytes, 16), extent};
}

}  // namespace tavern
