#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <filesystem>
#include <unordered_set>

#include "adjacency_tree.hpp"
#include "database_internal.hpp"
#include "directory.hpp"

namespace {

auto path(const char* suffix) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         (std::string("tavern-mutual-") +
          std::to_string(static_cast<long long>(getpid())) + suffix);
}

auto matches(const std::vector<tavern::PersonId>& actual,
             const std::unordered_set<tavern::PersonId>& expected) -> bool {
  return actual.size() == expected.size() &&
         std::unordered_set<tavern::PersonId>(actual.begin(), actual.end()) ==
             expected;
}

auto snapshot(const tavern::PageStore& store, std::vector<std::byte>& bytes)
    -> bool {
  bytes.resize(static_cast<std::size_t>(store.page_count) * tavern::page_size);
  return pread(store.file_descriptor, bytes.data(), bytes.size(), 0) ==
         static_cast<ssize_t>(bytes.size());
}

auto small_cases_and_corruption() -> bool {
  const auto file = path("-small.db");
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  if (!database || !tavern::mutual_friends(*database, 1, 2)->empty())
    return false;
  for (const tavern::PersonId id : {10U, 11U, 12U})
    if (!tavern::befriend(*database, 1, id)) return false;
  for (const tavern::PersonId id : {11U, 12U, 13U})
    if (!tavern::befriend(*database, 2, id)) return false;
  const auto partial = tavern::mutual_friends(*database, 1, 2);
  if (!partial || !matches(*partial, {11, 12})) return false;
  if (!tavern::befriend(*database, 1, 13)) return false;
  const auto identical = tavern::mutual_friends(*database, 1, 2);
  if (!identical || !matches(*identical, {11, 12, 13})) return false;
  if (!tavern::unfriend(*database, 2, 11) ||
      !tavern::unfriend(*database, 2, 12) ||
      !tavern::unfriend(*database, 2, 13) ||
      !tavern::befriend(*database, 2, 20) ||
      !tavern::mutual_friends(*database, 1, 2)->empty()) {
    return false;
  }
  if (!tavern::unfriend(*database, 2, 20) ||
      !tavern::befriend(*database, 2, 10))
    return false;
  std::vector<std::byte> before;
  std::vector<std::byte> after;
  const auto durable = tavern::mutual_friends(*database, 1, 2);
  if (!durable || !matches(*durable, {10}) ||
      !snapshot(database_impl(*database).store, before) ||
      !tavern::mutual_friends(*database, 1, 2) ||
      !snapshot(database_impl(*database).store, after) || before != after) {
    return false;
  }
  const auto record = tavern::directory_lookup(
      database_impl(*database).store, database_impl(*database).commit.root,
      database_impl(*database).commit.extent, 1);
  if (!record || !*record ||
      (**record).friends.root == tavern::invalid_page_id ||
      !tavern::close_database(*database)) {
    return false;
  }
  auto durable_reopened = tavern::open_database(file);
  const auto durable_after_reopen =
      durable_reopened
          ? tavern::mutual_friends(*durable_reopened, 1, 2)
          : std::expected<std::vector<tavern::PersonId>, tavern::Error>(
                std::unexpected(tavern::Error::io_failure));
  if (!durable_after_reopen || !matches(*durable_after_reopen, {10}) ||
      !durable_reopened || !tavern::close_database(*durable_reopened)) {
    return false;
  }
  const int descriptor = open(file.c_str(), O_RDWR);
  const off_t checksum =
      static_cast<off_t>((**record).friends.root * tavern::page_size + 12);
  std::byte byte{};
  const bool corrupted = descriptor >= 0 &&
                         pread(descriptor, &byte, 1, checksum) == 1 &&
                         ((byte ^= std::byte{0xFF}),
                          pwrite(descriptor, &byte, 1, checksum) == 1) &&
                         close(descriptor) == 0;
  auto reopened = tavern::open_database(file);
  const auto failed =
      reopened ? tavern::mutual_friends(*reopened, 1, 2)
               : std::expected<std::vector<tavern::PersonId>, tavern::Error>(
                     std::unexpected(tavern::Error::io_failure));
  const auto closed = reopened
                          ? tavern::close_database(*reopened)
                          : std::expected<void, tavern::Error>(
                                std::unexpected(tavern::Error::io_failure));
  std::filesystem::remove(file);
  return corrupted && !failed && closed.has_value();
}

auto build_large_fixture(tavern::Database& database, std::size_t degree,
                         std::vector<tavern::PersonId>& targets) -> bool {
  targets.reserve(degree);
  for (std::size_t index = 0; index < degree; ++index)
    targets.push_back(static_cast<tavern::PersonId>(10000U + index));
  const std::array<tavern::PersonId, 3> large_hubs{1, 2, 3};
  const std::array<tavern::PersonId, 2> small_hubs{1, 2};
  const auto large =
      tavern::build_adjacency_tree(database_impl(database).store, targets);
  const auto small = tavern::build_adjacency_tree(
      database_impl(database).store,
      std::span<const tavern::PersonId>(targets).first(10));
  const auto target_large =
      tavern::build_adjacency_tree(database_impl(database).store, large_hubs);
  const auto target_small =
      tavern::build_adjacency_tree(database_impl(database).store, small_hubs);
  if (!large || !small || !target_large || !target_small) return false;
  tavern::PageId root = database_impl(database).commit.root;
  const auto insert = [&](const tavern::PatronRecord& record) -> bool {
    const auto candidate =
        tavern::directory_insert(database_impl(database).store, root, record);
    if (!candidate) return false;
    root = *candidate;
    return true;
  };
  for (const tavern::PersonId id : {1U, 2U}) {
    tavern::PatronRecord hub{};
    hub.id = id;
    hub.friends = *large;
    if (!insert(hub)) return false;
  }
  tavern::PatronRecord unequal{};
  unequal.id = 3;
  unequal.friends = *small;
  if (!insert(unequal)) return false;
  for (std::size_t index = 0; index < targets.size(); ++index) {
    tavern::PatronRecord target{};
    target.id = targets[index];
    target.friends = index < 10 ? *target_large : *target_small;
    if (!insert(target)) return false;
  }
  return tavern::commit_root(database_impl(database).store,
                             database_impl(database).commit, root)
      .has_value();
}

auto large_durable_intersection() -> bool {
  const auto file = path("-large.db");
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  std::vector<tavern::PersonId> targets;
  if (!database || !build_large_fixture(*database, 10000, targets) ||
      !tavern::close_database(*database)) {
    return false;
  }
  auto reopened = tavern::open_database(file);
  if (!reopened) return false;
  std::vector<std::byte> before;
  std::vector<std::byte> after;
  const auto equal = tavern::mutual_friends(*reopened, 1, 2);
  const auto unequal = tavern::mutual_friends(*reopened, 1, 3);
  const std::unordered_set<tavern::PersonId> expected(targets.begin(),
                                                      targets.end());
  const std::unordered_set<tavern::PersonId> first_ten(targets.begin(),
                                                       targets.begin() + 10);
  const bool result = equal && unequal && matches(*equal, expected) &&
                      matches(*unequal, first_ten) &&
                      snapshot(database_impl(*reopened).store, before) &&
                      tavern::mutual_friends(*reopened, 1, 2) &&
                      snapshot(database_impl(*reopened).store, after) &&
                      before == after && tavern::close_database(*reopened);
  std::filesystem::remove(file);
  return result;
}

}  // namespace

auto main() -> int {
  return small_cases_and_corruption() && large_durable_intersection() ? 0 : 1;
}
