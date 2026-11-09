#include <unistd.h>

#include <array>
#include <filesystem>

#include "database_internal.hpp"
#include "reachability.hpp"

namespace {

auto path(const char* suffix) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         (std::string("tavern-follow-reuse-") +
          std::to_string(static_cast<long long>(getpid())) + suffix);
}

auto raw(const tavern::PageStore& store, tavern::PageId id,
         std::array<std::byte, tavern::page_size>& bytes) -> bool {
  return pread(store.file_descriptor, bytes.data(), bytes.size(),
               static_cast<off_t>(id * tavern::page_size)) ==
         static_cast<ssize_t>(bytes.size());
}

auto add_candidates(tavern::Database& database) -> bool {
  const std::array<std::byte, 1> payload{std::byte{0x4A}};
  for (int count = 0; count < 4; ++count)
    if (!tavern::append_page(database_impl(database).store,
                             tavern::PageType::data, payload))
      return false;
  return true;
}

auto successful_reuse() -> bool {
  const auto file = path("-success.db");
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  if (!database || !tavern::follow(*database, 1, 2) ||
      !add_candidates(*database))
    return false;
  const auto before =
      tavern::scan_reachable_pages(database_impl(*database).store);
  const auto old_root = database_impl(*database).commit.root;
  const auto old_extent = database_impl(*database).commit.extent;
  if (!before) return false;
  std::vector<tavern::PageId> candidates;
  for (tavern::PageId id = tavern::first_data_page;
       id < database_impl(*database).store.page_count; ++id)
    if (!before->reachable[static_cast<std::size_t>(id)])
      candidates.push_back(id);
  bool below_extent = false;
  for (const auto id : candidates)
    below_extent = below_extent || id < old_extent;
  if (candidates.size() < 4 || !below_extent) return false;
  std::vector<std::array<std::byte, tavern::page_size>> old_pages(
      candidates.size());
  for (std::size_t index = 0; index < candidates.size(); ++index)
    if (!raw(database_impl(*database).store, candidates[index],
             old_pages[index]))
      return false;
  const auto page_count = database_impl(*database).store.page_count;
  if (!tavern::follow(*database, 3, 4) ||
      database_impl(*database).store.page_count != page_count ||
      !tavern::is_following(*database, 3, 4))
    return false;
  bool reused = false;
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    std::array<std::byte, tavern::page_size> page{};
    if (!raw(database_impl(*database).store, candidates[index], page))
      return false;
    reused = reused || page != old_pages[index];
  }
  const auto protected_scan =
      tavern::scan_protected_directories(database_impl(*database).store);
  const auto generation = database_impl(*database).commit.generation;
  if (!reused || !protected_scan || protected_scan->roots.size() != 2 ||
      !protected_scan->directory_pages.contains(old_root) ||
      !tavern::close_database(*database))
    return false;
  auto reopened = tavern::open_database(file);
  const bool recovered =
      reopened && database_impl(*reopened).commit.generation == generation &&
      tavern::is_following(*reopened, 1, 2).value_or(false) &&
      tavern::is_following(*reopened, 3, 4).value_or(false);
  if (reopened) (void)tavern::close_database(*reopened);
  std::filesystem::remove(file);
  return recovered;
}

auto allocation_and_commit_failure() -> bool {
  const auto file = path("-failure.db");
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  if (!database || !tavern::follow(*database, 1, 2) ||
      !add_candidates(*database))
    return false;
  const auto generation = database_impl(*database).commit.generation;
  const auto root = database_impl(*database).commit.root;
  const auto page_count = database_impl(*database).store.page_count;
  database_impl(*database).store.fail_reusable_write_after_for_test = 1;
  if (tavern::follow(*database, 3, 4) ||
      database_impl(*database).commit.generation != generation ||
      database_impl(*database).commit.root != root ||
      database_impl(*database).store.page_count != page_count ||
      tavern::is_following(*database, 3, 4).value_or(true))
    return false;
  if (!tavern::close_database(*database)) return false;
  auto reopened = tavern::open_database(file);
  if (!reopened || !tavern::is_following(*reopened, 1, 2).value_or(false) ||
      tavern::is_following(*reopened, 3, 4).value_or(true))
    return false;
  database_impl(*reopened).store.fail_metadata_write_for_test = true;
  const bool commit_failure = !tavern::follow(*reopened, 3, 4) &&
                              database_impl(*reopened).commit.recovery_required;
  if (!tavern::close_database(*reopened)) return false;
  auto recovered = tavern::open_database(file);
  const bool preserved =
      recovered && tavern::is_following(*recovered, 1, 2).value_or(false) &&
      !tavern::is_following(*recovered, 3, 4).value_or(true);
  if (recovered) (void)tavern::close_database(*recovered);
  std::filesystem::remove(file);
  return commit_failure && preserved;
}

}  // namespace

auto main() -> int {
  return successful_reuse() && allocation_and_commit_failure() ? 0 : 1;
}
