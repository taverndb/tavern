#include "free_page_pool.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <filesystem>
#include <unordered_set>

#include "reachability.hpp"

namespace {

auto path(const char* suffix) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         (std::string("tavern-free-pool-") +
          std::to_string(static_cast<long long>(getpid())) + suffix);
}

auto file_size(const std::filesystem::path& file, off_t& result) -> bool {
  struct stat status{};
  return stat(file.c_str(), &status) == 0 ? (result = status.st_size, true)
                                          : false;
}

auto snapshot(const tavern::Database& database, std::vector<std::byte>& bytes)
    -> bool {
  bytes.resize(
      static_cast<std::size_t>(database_impl(database).store.page_count) *
      tavern::page_size);
  return pread(database_impl(database).store.file_descriptor, bytes.data(),
               bytes.size(), 0) == static_cast<ssize_t>(bytes.size());
}

auto unreachable_pages(const tavern::Database& database)
    -> std::expected<std::vector<tavern::PageId>, tavern::Error> {
  const auto scan = tavern::scan_reachable_pages(database_impl(database).store);
  if (!scan) return std::unexpected(scan.error());
  std::vector<tavern::PageId> pages;
  for (tavern::PageId id = tavern::first_data_page;
       id < database_impl(database).store.page_count; ++id) {
    if (!scan->reachable[static_cast<std::size_t>(id)]) pages.push_back(id);
  }
  return pages;
}

auto exact_pool(const tavern::Database& database,
                const tavern::FreePagePool& pool) -> bool {
  const auto expected = unreachable_pages(database);
  if (!expected || pool.candidates != *expected ||
      pool.candidates.size() !=
          std::unordered_set<tavern::PageId>(pool.candidates.begin(),
                                             pool.candidates.end())
              .size())
    return false;
  for (const tavern::PageId id : pool.candidates)
    if (id < tavern::first_data_page) return false;
  return true;
}

auto empty_and_relationship_pool() -> bool {
  const auto file = path("-normal.db");
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  if (!database) return false;
  off_t before = 0;
  off_t after = 0;
  if (!file_size(file, before)) return false;
  const auto empty = tavern::build_free_page_pool(*database);
  if (!empty || !empty->candidates.empty() ||
      !tavern::free_page_pool_matches(*empty, *database) ||
      !file_size(file, after) || before != after)
    return false;
  if (!tavern::follow(*database, 1, 2) || !tavern::befriend(*database, 3, 4))
    return false;
  const std::array<std::byte, 1> payload{std::byte{1}};
  if (!tavern::append_page(database_impl(*database).store,
                           tavern::PageType::data, payload))
    return false;
  if (!file_size(file, before)) return false;
  const auto pool = tavern::build_free_page_pool(*database);
  const bool result = pool && exact_pool(*database, *pool) &&
                      tavern::free_page_pool_matches(*pool, *database) &&
                      file_size(file, after) && before == after;
  const auto closed = tavern::close_database(*database);
  std::filesystem::remove(file);
  return result && closed.has_value();
}

auto stale_and_failed_candidate_pool() -> bool {
  const auto file = path("-stale.db");
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  if (!database || !tavern::follow(*database, 1, 2)) return false;
  const auto pool = tavern::build_free_page_pool(*database);
  if (!pool || !tavern::free_page_pool_matches(*pool, *database)) return false;
  const std::array<std::byte, 1> payload{std::byte{2}};
  if (!tavern::append_page(database_impl(*database).store,
                           tavern::PageType::data, payload) ||
      tavern::free_page_pool_matches(*pool, *database))
    return false;
  const auto appended_pool = tavern::build_free_page_pool(*database);
  if (!appended_pool ||
      !tavern::free_page_pool_matches(*appended_pool, *database) ||
      !tavern::follow(*database, 3, 4) ||
      tavern::free_page_pool_matches(*appended_pool, *database))
    return false;
  database_impl(*database).store.fail_append_after_for_test = 2;
  if (tavern::befriend(*database, 5, 6) ||
      database_impl(*database).commit.recovery_required)
    return false;
  const auto failed_pool = tavern::build_free_page_pool(*database);
  const bool result = failed_pool && !failed_pool->candidates.empty() &&
                      exact_pool(*database, *failed_pool);
  const auto closed = tavern::close_database(*database);
  std::filesystem::remove(file);
  return result && closed.has_value();
}

auto invalid_handles_and_corruption() -> bool {
  const auto file = path("-invalid.db");
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  if (!database || !tavern::follow(*database, 1, 2)) return false;
  const auto pool = tavern::build_free_page_pool(*database);
  const auto other = tavern::open_database(file);
  if (!pool || other || other.error() != tavern::Error::database_locked)
    return false;
  const tavern::PageId root = database_impl(*database).commit.root;
  const std::byte corrupt{0xA5};
  if (pwrite(database_impl(*database).store.file_descriptor, &corrupt, 1,
             static_cast<off_t>(root * tavern::page_size)) != 1 ||
      tavern::build_free_page_pool(*database))
    return false;
  if (!tavern::close_database(*database)) return false;
  std::filesystem::remove(file);
  return !tavern::build_free_page_pool(*database);
}

auto reservation() -> bool {
  const auto file = path("-reservation.db");
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  if (!database) return false;
  auto empty = tavern::build_free_page_pool(*database);
  const auto no_page =
      empty ? tavern::reserve_free_page(*empty, *database)
            : std::expected<std::optional<tavern::PageId>, tavern::Error>(
                  std::unexpected(tavern::Error::invalid_page));
  if (!no_page || *no_page) return false;
  if (!tavern::follow(*database, 1, 2) || !tavern::befriend(*database, 3, 4))
    return false;
  const std::array<std::byte, 1> payload{std::byte{3}};
  if (!tavern::append_page(database_impl(*database).store,
                           tavern::PageType::data, payload) ||
      !tavern::append_page(database_impl(*database).store,
                           tavern::PageType::data, payload))
    return false;
  auto pool = tavern::build_free_page_pool(*database);
  if (!pool || pool->candidates.size() < 2 || !exact_pool(*database, *pool))
    return false;
  const auto expected = pool->candidates;
  std::vector<std::byte> before;
  std::vector<std::byte> after;
  if (!snapshot(*database, before)) return false;
  const auto first = tavern::reserve_free_page(*pool, *database);
  database_impl(*database).store.fail_reusable_write_after_for_test = 0;
  if (!first || !*first || **first != expected.front() ||
      tavern::follow(*database, 5, 6))
    return false;
  const auto second = tavern::reserve_free_page(*pool, *database);
  if (!second || (expected.size() > 1 && (!*second || **second != expected[1])))
    return false;
  std::unordered_set<tavern::PageId> reserved;
  reserved.insert(**first);
  if (*second) reserved.insert(**second);
  while (true) {
    const auto page = tavern::reserve_free_page(*pool, *database);
    if (!page) return false;
    if (!*page) break;
    if (!reserved.insert(**page).second || **page < tavern::first_data_page)
      return false;
  }
  if (reserved.size() != expected.size() || !snapshot(*database, after) ||
      before != after)
    return false;
  auto stale = tavern::build_free_page_pool(*database);
  if (!stale || !tavern::follow(*database, 7, 8) ||
      tavern::reserve_free_page(*stale, *database))
    return false;
  auto owner_pool = tavern::build_free_page_pool(*database);
  const auto other = tavern::open_database(file);
  if (!owner_pool || other || other.error() != tavern::Error::database_locked)
    return false;
  const auto closed = tavern::close_database(*database);
  const auto closed_reservation =
      tavern::reserve_free_page(*owner_pool, *database);
  std::filesystem::remove(file);
  return closed.has_value() && !closed_reservation;
}

}  // namespace

auto main() -> int {
  return empty_and_relationship_pool() && stale_and_failed_candidate_pool() &&
                 invalid_handles_and_corruption() && reservation()
             ? 0
             : 1;
}
