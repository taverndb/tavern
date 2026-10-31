#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <filesystem>
#include <iostream>
#include <string>

#include "database_internal.hpp"

namespace {

struct TemporaryFile {
  std::filesystem::path path;
  ~TemporaryFile() { std::filesystem::remove(path); }
};

auto unique_path(const char* suffix) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         (std::string("tavern-root-corruption-") +
          std::to_string(static_cast<long long>(getpid())) + suffix);
}

auto create_committed_database(const std::filesystem::path& path,
                               tavern::PageId& root) -> bool {
  std::filesystem::remove(path);
  auto database = tavern::create_database(path);
  if (!database || !tavern::follow(*database, 1, 2)) {
    return false;
  }
  root = database_impl(*database).commit.root;
  return tavern::close_database(*database).has_value();
}

auto checksum(const std::array<std::byte, tavern::page_size>& page)
    -> std::uint64_t {
  std::uint64_t value = 14695981039346656037ULL;
  for (std::size_t index = 20; index < page.size(); ++index) {
    value ^= std::to_integer<unsigned int>(page[index]);
    value *= 1099511628211ULL;
  }
  return value;
}

void write_u16(std::array<std::byte, tavern::page_size>& page,
               std::size_t offset, std::uint16_t value) {
  page[offset] = static_cast<std::byte>(value);
  page[offset + 1] = static_cast<std::byte>(value >> 8U);
}

void write_u64(std::array<std::byte, tavern::page_size>& page,
               std::size_t offset, std::uint64_t value) {
  for (std::size_t index = 0; index < 8; ++index) {
    page[offset + index] = static_cast<std::byte>(value >> (index * 8U));
  }
}

auto metadata_checksum(const std::array<std::byte, tavern::page_size>& page)
    -> std::uint64_t {
  std::uint64_t value = 14695981039346656037ULL;
  for (std::size_t index = 20; index < 52; ++index) {
    value ^= std::to_integer<unsigned int>(page[index]);
    value *= 1099511628211ULL;
  }
  return value;
}

auto read_raw_page(int descriptor, tavern::PageId id,
                   std::array<std::byte, tavern::page_size>& page) -> bool {
  return pread(descriptor, page.data(), page.size(),
               static_cast<off_t>(id * tavern::page_size)) ==
         static_cast<ssize_t>(page.size());
}

auto write_raw_page(int descriptor, tavern::PageId id,
                    const std::array<std::byte, tavern::page_size>& page)
    -> bool {
  return pwrite(descriptor, page.data(), page.size(),
                static_cast<off_t>(id * tavern::page_size)) ==
         static_cast<ssize_t>(page.size());
}

auto test_invalid_root_checksum() -> bool {
  TemporaryFile file{unique_path("-checksum.db")};
  tavern::PageId root = tavern::invalid_page_id;
  if (!create_committed_database(file.path, root)) {
    return false;
  }
  const int descriptor = open(file.path.c_str(), O_RDWR);
  if (descriptor < 0) {
    return false;
  }
  const std::byte corrupt{0xA5};
  const bool wrote =
      pwrite(descriptor, &corrupt, 1,
             static_cast<off_t>(root * tavern::page_size + 12)) == 1;
  const bool closed = close(descriptor) == 0;
  return wrote && closed && !tavern::open_database(file.path).has_value();
}

auto test_invalid_root_type() -> bool {
  TemporaryFile file{unique_path("-type.db")};
  tavern::PageId root = tavern::invalid_page_id;
  if (!create_committed_database(file.path, root)) {
    return false;
  }
  const int descriptor = open(file.path.c_str(), O_RDWR);
  std::array<std::byte, tavern::page_size> page{};
  if (descriptor < 0 || !read_raw_page(descriptor, root, page)) {
    if (descriptor >= 0) {
      (void)close(descriptor);
    }
    return false;
  }
  write_u16(page, 6, static_cast<std::uint16_t>(tavern::PageType::data));
  write_u64(page, 12, checksum(page));
  const bool wrote = write_raw_page(descriptor, root, page);
  const bool closed = close(descriptor) == 0;
  return wrote && closed && !tavern::open_database(file.path).has_value();
}

auto test_invalid_committed_extent() -> bool {
  TemporaryFile file{unique_path("-extent.db")};
  tavern::PageId root = tavern::invalid_page_id;
  if (!create_committed_database(file.path, root)) return false;
  const int descriptor = open(file.path.c_str(), O_RDWR);
  struct stat status{};
  std::array<std::byte, tavern::page_size> metadata{};
  if (descriptor < 0 || fstat(descriptor, &status) != 0 ||
      !read_raw_page(descriptor, tavern::metadata_slot_b, metadata)) {
    if (descriptor >= 0) {
      (void)close(descriptor);
    }
    return false;
  }
  write_u64(metadata, 44,
            static_cast<std::uint64_t>(status.st_size /
                                       static_cast<off_t>(tavern::page_size)) +
                1);
  write_u64(metadata, 52, metadata_checksum(metadata));
  write_u64(metadata, 12, checksum(metadata));
  const bool wrote =
      write_raw_page(descriptor, tavern::metadata_slot_b, metadata);
  const bool closed = close(descriptor) == 0;
  return wrote && closed && !tavern::open_database(file.path).has_value();
}

auto test_truncated_root() -> bool {
  TemporaryFile file{unique_path("-truncated.db")};
  tavern::PageId root = tavern::invalid_page_id;
  if (!create_committed_database(file.path, root)) return false;
  const int descriptor = open(file.path.c_str(), O_RDWR);
  const bool truncated =
      descriptor >= 0 &&
      ftruncate(descriptor, static_cast<off_t>(tavern::first_data_page *
                                               tavern::page_size)) == 0;
  const bool closed = descriptor >= 0 && close(descriptor) == 0;
  return truncated && closed && !tavern::open_database(file.path).has_value();
}

auto test_unreachable_trailing_pages() -> bool {
  TemporaryFile file{unique_path("-trailing.db")};
  tavern::PageId root = tavern::invalid_page_id;
  if (!create_committed_database(file.path, root)) return false;
  auto store = tavern::open_page_store(file.path);
  if (!store) return false;
  const tavern::PageId original_end = store->page_count;
  const std::array<std::byte, 1> payload{std::byte{0x44}};
  const auto abandoned =
      tavern::append_page(*store, tavern::PageType::data, payload);
  if (!abandoned || *abandoned != original_end ||
      !tavern::close_page_store(*store))
    return false;
  auto database = tavern::open_database(file.path);
  if (!database || database_impl(*database).commit.root != root ||
      !tavern::is_following(*database, 1, 2).value())
    return false;
  if (!tavern::follow(*database, 2, 3)) return false;
  const bool appended_after_abandoned =
      database_impl(*database).store.page_count > original_end + 1;
  if (!tavern::close_database(*database)) return false;
  auto reopened = tavern::open_database(file.path);
  const bool valid = reopened &&
                     tavern::is_following(*reopened, 1, 2).value() &&
                     tavern::is_following(*reopened, 2, 3).value();
  if (reopened) (void)tavern::close_database(*reopened);
  return appended_after_abandoned && valid;
}

auto test_unsupported_format_version() -> bool {
  TemporaryFile file{unique_path("-version.db")};
  tavern::PageId root = tavern::invalid_page_id;
  if (!create_committed_database(file.path, root)) return false;
  const int descriptor = open(file.path.c_str(), O_RDWR);
  if (descriptor < 0) return false;
  bool valid = true;
  for (const tavern::PageId slot :
       {tavern::metadata_slot_a, tavern::metadata_slot_b}) {
    std::array<std::byte, tavern::page_size> page{};
    valid = valid && read_raw_page(descriptor, slot, page);
    write_u16(page, 4, 99);
    write_u16(page, 24, 99);
    write_u64(page, 52, metadata_checksum(page));
    write_u64(page, 12, checksum(page));
    valid = valid && write_raw_page(descriptor, slot, page);
  }
  valid = valid && close(descriptor) == 0;
  return valid && !tavern::open_database(file.path).has_value();
}

}  // namespace

auto main() -> int {
  const bool checksum_rejected = test_invalid_root_checksum();
  const bool type_rejected = test_invalid_root_type();
  const bool extent_rejected = test_invalid_committed_extent();
  const bool truncation_rejected = test_truncated_root();
  const bool trailing_preserved = test_unreachable_trailing_pages();
  const bool version_rejected = test_unsupported_format_version();
  if (!checksum_rejected || !type_rejected || !extent_rejected ||
      !truncation_rejected || !trailing_preserved || !version_rejected) {
    std::cerr << "root corruption recovery test failed\n";
    return 1;
  }
  return 0;
}
