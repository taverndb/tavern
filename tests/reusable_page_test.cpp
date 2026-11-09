#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <filesystem>

#include "free_page_pool.hpp"
#include "page_store_internal.hpp"

namespace {

auto path() -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         (std::string("tavern-reusable-page-") +
          std::to_string(static_cast<long long>(getpid())) + ".db");
}

auto read_raw(const tavern::PageStore& store, tavern::PageId id,
              std::array<std::byte, tavern::page_size>& page) -> bool {
  return pread(store.file_descriptor, page.data(), page.size(),
               static_cast<off_t>(id * tavern::page_size)) ==
         static_cast<ssize_t>(page.size());
}

auto file_size(const std::filesystem::path& file, off_t& result) -> bool {
  struct stat status{};
  return stat(file.c_str(), &status) == 0 ? (result = status.st_size, true)
                                          : false;
}

auto reserve_not(tavern::FreePagePool& pool, const tavern::Database& database,
                 tavern::PageId excluded)
    -> std::expected<tavern::PageId, tavern::Error> {
  while (true) {
    const auto page = tavern::reserve_free_page(pool, database);
    if (!page) return std::unexpected(page.error());
    if (!*page) return std::unexpected(tavern::Error::invalid_page);
    if (**page != excluded) return **page;
  }
}

auto reusable_page_write() -> bool {
  const auto file = path();
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  if (!database || !tavern::follow(*database, 1, 2)) return false;
  const std::array<std::byte, 2> payload{std::byte{0x12}, std::byte{0x34}};
  const auto source = tavern::append_page(database_impl(*database).store,
                                          tavern::PageType::data, payload);
  if (!source ||
      !tavern::append_page(database_impl(*database).store,
                           tavern::PageType::data, payload) ||
      !tavern::append_page(database_impl(*database).store,
                           tavern::PageType::data, payload) ||
      !tavern::append_page(database_impl(*database).store,
                           tavern::PageType::data, payload))
    return false;
  std::array<std::byte, tavern::page_size> encoded{};
  if (!read_raw(database_impl(*database).store, *source, encoded)) return false;
  auto pool = tavern::build_free_page_pool(*database);
  if (!pool || pool->candidates.size() < 5) return false;
  const auto destination = reserve_not(*pool, *database, *source);
  if (!destination) return false;
  off_t before = 0;
  off_t after = 0;
  if (!file_size(file, before) || !tavern::is_following(*database, 1, 2))
    return false;
  const auto written =
      tavern::storage_detail::overwrite_reserved_unreachable_page(
          database_impl(*database).store, *destination, encoded);
  if (!written || !file_size(file, after) || before != after ||
      !tavern::is_following(*database, 1, 2))
    return false;
  std::array<std::byte, tavern::page_size> read_back{};
  if (!read_raw(database_impl(*database).store, *destination, read_back) ||
      read_back != encoded)
    return false;
  auto malformed = encoded;
  malformed[12] ^= std::byte{0xFF};
  if (tavern::storage_detail::overwrite_reserved_unreachable_page(
          database_impl(*database).store, tavern::metadata_slot_a, encoded) ||
      tavern::storage_detail::overwrite_reserved_unreachable_page(
          database_impl(*database).store,
          database_impl(*database).store.page_count, encoded) ||
      tavern::storage_detail::overwrite_reserved_unreachable_page(
          database_impl(*database).store, *destination,
          std::span<const std::byte>(encoded).first(tavern::page_size - 1)) ||
      tavern::storage_detail::overwrite_reserved_unreachable_page(
          database_impl(*database).store, *destination, malformed))
    return false;
  const auto failed = reserve_not(*pool, *database, *source);
  if (!failed) return false;
  database_impl(*database).store.fail_reusable_write_after_for_test = 0;
  if (tavern::storage_detail::overwrite_reserved_unreachable_page(
          database_impl(*database).store, *failed, encoded))
    return false;
  const auto after_failed = reserve_not(*pool, *database, *source);
  if (!after_failed || *after_failed == *failed) return false;
  database_impl(*database).store.fail_reusable_write_after_for_test = 1;
  if (tavern::storage_detail::overwrite_reserved_unreachable_page(
          database_impl(*database).store, *after_failed, encoded))
    return false;
  const auto after_partial = reserve_not(*pool, *database, *source);
  if (!after_partial || *after_partial == *after_failed ||
      !tavern::is_following(*database, 1, 2))
    return false;
  const auto generation = database_impl(*database).commit.generation;
  if (!tavern::close_database(*database) ||
      tavern::storage_detail::overwrite_reserved_unreachable_page(
          database_impl(*database).store, *destination, encoded))
    return false;
  auto reopened = tavern::open_database(file);
  const bool recovered =
      reopened && database_impl(*reopened).commit.generation == generation &&
      tavern::is_following(*reopened, 1, 2).value_or(false);
  if (reopened) (void)tavern::close_database(*reopened);
  std::filesystem::remove(file);
  return recovered;
}

}  // namespace

auto main() -> int { return reusable_page_write() ? 0 : 1; }
