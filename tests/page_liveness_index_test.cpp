#include "page_liveness_index.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <filesystem>
#include <unordered_set>

#include "adjacency_tree.hpp"
#include "database_internal.hpp"
#include "directory.hpp"
#include "persistent_relationship.hpp"
#include "reachability.hpp"

namespace {

auto path(const char* suffix) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         (std::string("tavern-liveness-") +
          std::to_string(static_cast<long long>(getpid())) + suffix);
}

auto exact(const tavern::PageLivenessIndex& index,
           const tavern::ReachabilityScan& scan) -> bool {
  if (index.physical_page_count != scan.reachable.size()) return false;
  std::unordered_set<tavern::PageId> free(index.reclaimable.begin(),
                                          index.reclaimable.end());
  for (tavern::PageId id = tavern::first_data_page;
       id < index.physical_page_count; ++id) {
    const bool live = index.live(id);
    if (live != scan.reachable[static_cast<std::size_t>(id)] ||
        (!live) != free.contains(id)) {
      return false;
    }
  }
  return true;
}

auto virtual_reachable(const tavern::PageStore& store,
                       const std::vector<tavern::PageLivenessRoot>& roots)
    -> std::expected<std::vector<bool>, tavern::Error> {
  std::vector<bool> live(static_cast<std::size_t>(store.page_count));
  for (const auto& root : roots) {
    const auto traversed = tavern::traverse_directory(
        store, root.root, root.extent,
        [&](tavern::PageId page,
            tavern::PageType) -> std::expected<void, tavern::Error> {
          live[static_cast<std::size_t>(page)] = true;
          return std::expected<void, tavern::Error>{};
        },
        [&](const tavern::PatronRecord& record)
            -> std::expected<void, tavern::Error> {
          for (const tavern::AdjacencyDescriptor& descriptor :
               {record.following, record.followers, record.friends}) {
            const auto adjacency = tavern::traverse_adjacency_pages(
                store, descriptor, root.extent,
                [&](tavern::PageId page,
                    tavern::PageType) -> std::expected<void, tavern::Error> {
                  live[static_cast<std::size_t>(page)] = true;
                  return std::expected<void, tavern::Error>{};
                });
            if (!adjacency) return adjacency;
          }
          return std::expected<void, tavern::Error>{};
        });
    if (!traversed) return std::unexpected(traversed.error());
  }
  return live;
}

auto simple_cases() -> bool {
  const auto file = path("-simple.db");
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  if (!database) return false;
  auto index =
      tavern::build_page_liveness_index(database_impl(*database).store);
  auto scan = tavern::scan_reachable_pages(database_impl(*database).store);
  if (!index || !scan || !exact(*index, *scan) || !index->reclaimable.empty())
    return false;
  if (!tavern::follow(*database, 1, 2) || !tavern::befriend(*database, 1, 3) ||
      !tavern::follow(*database, 4, 1))
    return false;
  index = tavern::build_page_liveness_index(database_impl(*database).store);
  scan = tavern::scan_reachable_pages(database_impl(*database).store);
  const bool result = index && scan && exact(*index, *scan) &&
                      index->protected_roots.size() == 2 &&
                      tavern::close_database(*database).has_value();
  std::filesystem::remove(file);
  return result;
}

auto build_shared_fixture(tavern::Database& database, std::size_t degree,
                          tavern::PageId& shared_root) -> bool {
  std::vector<tavern::PersonId> ids;
  ids.reserve(degree);
  for (std::size_t index = 0; index < degree; ++index)
    ids.push_back(static_cast<tavern::PersonId>(1000U + index));
  const auto shared =
      tavern::build_adjacency_tree(database_impl(database).store, ids);
  if (!shared) return false;
  shared_root = shared->root;
  tavern::PageId root = database_impl(database).commit.root;
  for (const tavern::PersonId id : {1U, 2U}) {
    tavern::PatronRecord record{};
    record.id = id;
    record.friends = *shared;
    const auto next =
        tavern::directory_insert(database_impl(database).store, root, record);
    if (!next) return false;
    root = *next;
  }
  for (const tavern::PersonId id : ids) {
    tavern::PatronRecord record{};
    record.id = id;
    const auto next =
        tavern::directory_insert(database_impl(database).store, root, record);
    if (!next) return false;
    root = *next;
  }
  return tavern::commit_root(database_impl(database).store,
                             database_impl(database).commit, root)
      .has_value();
}

auto identical_descriptors_share_one_parent_reference() -> bool {
  const auto file = path("-identical.db");
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  tavern::PageId shared_root = tavern::invalid_page_id;
  if (!database || !build_shared_fixture(*database, 1, shared_root))
    return false;
  const auto index =
      tavern::build_page_liveness_index(database_impl(*database).store);
  const bool result =
      index &&
      index->incoming_references[static_cast<std::size_t>(shared_root)] == 1 &&
      tavern::close_database(*database);
  std::filesystem::remove(file);
  return result;
}

auto sharing_high_degree_reopen_and_unreachable() -> bool {
  const auto file = path("-shared.db");
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  tavern::PageId shared_root = tavern::invalid_page_id;
  if (!database || !build_shared_fixture(*database, 1000, shared_root) ||
      !tavern::follow(*database, 1, 2)) {
    return false;
  }
  const std::array<std::byte, 1> payload{std::byte{0x4D}};
  if (!tavern::append_page(database_impl(*database).store,
                           tavern::PageType::data, payload))
    return false;
  const auto index =
      tavern::build_page_liveness_index(database_impl(*database).store);
  const auto scan =
      tavern::scan_reachable_pages(database_impl(*database).store);
  if (!index || !scan || !exact(*index, *scan) ||
      index->incoming_references[static_cast<std::size_t>(shared_root)] < 2 ||
      index->memory_bytes() == 0 || !tavern::close_database(*database)) {
    return false;
  }
  auto reopened = tavern::open_database(file);
  const auto reopened_index =
      reopened
          ? tavern::build_page_liveness_index(database_impl(*reopened).store)
          : std::expected<tavern::PageLivenessIndex, tavern::Error>(
                std::unexpected(tavern::Error::io_failure));
  const auto reopened_scan =
      reopened ? tavern::scan_reachable_pages(database_impl(*reopened).store)
               : std::expected<tavern::ReachabilityScan, tavern::Error>(
                     std::unexpected(tavern::Error::io_failure));
  const bool result = reopened_index && reopened_scan &&
                      exact(*reopened_index, *reopened_scan) && reopened &&
                      tavern::close_database(*reopened);
  std::filesystem::remove(file);
  return result;
}

auto simulate_replacement() -> bool {
  const auto file = path("-simulate.db");
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  if (!database || !tavern::follow(*database, 1, 2) ||
      !tavern::befriend(*database, 1, 3) || !tavern::follow(*database, 4, 1))
    return false;
  const tavern::PageId overwritten =
      database_impl(*database).commit.active_slot == tavern::metadata_slot_a
          ? tavern::metadata_slot_b
          : tavern::metadata_slot_a;
  const auto candidate = tavern::persistent_follow(
      database_impl(*database).store, database_impl(*database).commit.root,
      database_impl(*database).commit.extent, 2, 4);
  if (!candidate) return false;
  auto index =
      tavern::build_page_liveness_index(database_impl(*database).store);
  if (!index || !tavern::simulate_page_liveness_root_replacement(
                    *index, database_impl(*database).store, overwritten,
                    *candidate, database_impl(*database).store.page_count)) {
    return false;
  }
  const auto expected =
      virtual_reachable(database_impl(*database).store, index->protected_roots);
  bool equal =
      expected && expected->size() == index->incoming_references.size();
  if (equal)
    for (tavern::PageId id = tavern::first_data_page;
         id < database_impl(*database).store.page_count; ++id)
      equal =
          equal && index->live(id) == (*expected)[static_cast<std::size_t>(id)];
  const bool closed = tavern::close_database(*database).has_value();
  std::filesystem::remove(file);
  return equal && closed;
}

auto corrupted_page_fails_closed() -> bool {
  const auto file = path("-corrupt.db");
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  if (!database || !tavern::follow(*database, 1, 2)) return false;
  const tavern::PageId root = database_impl(*database).commit.root;
  if (!tavern::close_database(*database)) return false;
  const int descriptor = open(file.c_str(), O_RDWR);
  const off_t offset = static_cast<off_t>(root * tavern::page_size + 12);
  std::byte value{};
  const bool corrupted = descriptor >= 0 &&
                         pread(descriptor, &value, 1, offset) == 1 &&
                         ((value ^= std::byte{0xFF}),
                          pwrite(descriptor, &value, 1, offset) == 1) &&
                         close(descriptor) == 0;
  auto reopened = tavern::open_database(file);
  const auto index =
      reopened
          ? tavern::build_page_liveness_index(database_impl(*reopened).store)
          : std::expected<tavern::PageLivenessIndex, tavern::Error>(
                std::unexpected(tavern::Error::io_failure));
  const auto closed = reopened
                          ? tavern::close_database(*reopened)
                          : std::expected<void, tavern::Error>(
                                std::unexpected(tavern::Error::io_failure));
  std::filesystem::remove(file);
  return corrupted && (!reopened || !index) &&
         (!reopened || closed.has_value());
}

}  // namespace

auto main() -> int {
  return simple_cases() && identical_descriptors_share_one_parent_reference() &&
                 sharing_high_degree_reopen_and_unreachable() &&
                 simulate_replacement() && corrupted_page_fails_closed()
             ? 0
             : 1;
}
