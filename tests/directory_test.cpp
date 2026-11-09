#include "directory.hpp"

#include <unistd.h>

#include <array>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>

namespace {
int checks = 0;
int failures = 0;
void check(bool value, const char* text, int line) {
  ++checks;
  if (!value) {
    ++failures;
    std::cerr << line << ": " << text << '\n';
  }
}
#define CHECK(x) check(static_cast<bool>(x), #x, __LINE__)

auto path() -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         ("tavern-directory-" +
          std::to_string(static_cast<long long>(getpid())) + ".db");
}

auto record(tavern::PersonId id, std::uint64_t count = 0)
    -> tavern::PatronRecord {
  tavern::PatronRecord value{};
  value.id = id;
  value.following = {id + 10, 2, count};
  value.followers = {id + 20, 3, count + 1};
  value.friends = {id + 30, 4, count + 2};
  return value;
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

auto get64(const std::array<std::byte, tavern::page_size>& page,
           std::size_t offset) -> std::uint64_t {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index)
    value |= static_cast<std::uint64_t>(
                 std::to_integer<unsigned long long>(page[offset + index]))
             << (index * 8U);
  return value;
}

void put64(std::array<std::byte, tavern::page_size>& page, std::size_t offset,
           std::uint64_t value) {
  for (std::size_t index = 0; index < 8; ++index)
    page[offset + index] = static_cast<std::byte>(value >> (index * 8U));
}

void test_basics_and_cow() {
  const auto file = path();
  std::filesystem::remove(file);
  auto store = tavern::create_page_store(file);
  CHECK(store.has_value());
  if (!store) return;
  tavern::PageId root = tavern::invalid_page_id;
  CHECK(!tavern::directory_lookup(*store, root, store->page_count, 1)
             .value()
             .has_value());
  const auto first = tavern::directory_insert(*store, root, record(1, 4));
  CHECK(first.has_value());
  if (!first) return;
  root = *first;
  const auto old_root = root;
  auto found = tavern::directory_lookup(*store, root, store->page_count, 1);
  CHECK(found.has_value() && found->has_value());
  CHECK(found && found->has_value() && found->value().following.count == 4);
  const auto updated = tavern::directory_update(*store, root, record(1, 99));
  CHECK(updated.has_value());
  if (!updated) return;
  CHECK(tavern::directory_lookup(*store, old_root, store->page_count, 1)
            ->value()
            .following.count == 4);
  root = *updated;
  CHECK(tavern::directory_lookup(*store, root, store->page_count, 1)
            ->value()
            .following.count == 99);
  CHECK(!tavern::directory_insert(*store, root, record(1)).has_value());
  const auto removed = tavern::directory_remove(*store, root, 1);
  CHECK(removed.has_value());
  CHECK(removed && *removed == tavern::invalid_page_id);
  CHECK(!tavern::directory_remove(*store, root, 99).has_value());
  CHECK(tavern::sync_page_store(*store).has_value());
  CHECK(tavern::close_page_store(*store).has_value());
  auto reopened = tavern::open_page_store(file);
  CHECK(reopened.has_value());
  if (reopened) {
    CHECK(tavern::directory_lookup(*reopened, root, reopened->page_count, 1)
              ->value()
              .following.count == 99);
    CHECK(tavern::close_page_store(*reopened).has_value());
  }
  std::filesystem::remove(file);
}

void test_splits_and_random() {
  const auto file = path();
  std::filesystem::remove(file);
  auto store = tavern::create_page_store(file);
  CHECK(store.has_value());
  if (!store) return;
  tavern::PageId root = tavern::invalid_page_id;
  std::map<tavern::PersonId, tavern::PatronRecord> reference;
  for (tavern::PersonId id = 0; id < 300; ++id) {
    const auto next = tavern::directory_insert(*store, root, record(id, id));
    CHECK(next.has_value());
    if (!next) return;
    root = *next;
    reference.emplace(id, record(id, id));
  }
  for (const auto& [id, expected] : reference) {
    const auto got =
        tavern::directory_lookup(*store, root, store->page_count, id);
    CHECK(got.has_value() && got->has_value());
    CHECK(got && got->has_value() &&
          got->value().friends.count == expected.friends.count);
  }
  for (tavern::PersonId id = 0; id < 300; id += 3) {
    const auto next = tavern::directory_remove(*store, root, id);
    CHECK(next.has_value());
    if (!next) return;
    root = *next;
    reference.erase(id);
  }
  for (tavern::PersonId id = 0; id < 300; ++id) {
    const auto got =
        tavern::directory_lookup(*store, root, store->page_count, id);
    CHECK(got.has_value());
    CHECK(got && got->has_value() == reference.contains(id));
  }
  const std::byte corrupt{0x99};
  CHECK(pwrite(store->file_descriptor, &corrupt, 1,
               static_cast<off_t>(root * tavern::page_size)) == 1);
  CHECK(!tavern::directory_lookup(*store, root, store->page_count, 1)
             .has_value());
  CHECK(tavern::close_page_store(*store).has_value());
  std::filesystem::remove(file);
}

void test_traversal_extent() {
  const auto file = path();
  std::filesystem::remove(file);
  auto store = tavern::create_page_store(file);
  CHECK(store.has_value());
  if (!store) return;
  tavern::PageId root = tavern::invalid_page_id;
  for (tavern::PersonId id = 0; id < 300; ++id) {
    const auto next = tavern::directory_insert(*store, root, record(id));
    CHECK(next.has_value());
    if (!next) return;
    root = *next;
  }
  const auto callback =
      [](tavern::PageId,
         tavern::PageType) -> std::expected<void, tavern::Error> {
    return std::expected<void, tavern::Error>{};
  };
  const auto records =
      [](const tavern::PatronRecord&) -> std::expected<void, tavern::Error> {
    return std::expected<void, tavern::Error>{};
  };
  CHECK(tavern::traverse_directory(*store, root, store->page_count, callback,
                                   records)
            .has_value());
  CHECK(!tavern::traverse_directory(*store, root, root, callback, records)
             .has_value());
  CHECK(
      tavern::directory_lookup(*store, root, store->page_count, 1).has_value());
  CHECK(!tavern::directory_lookup(*store, root, root, 1).has_value());
  const auto trailing = tavern::append_page(
      *store, tavern::PageType::data, std::array<std::byte, 1>{std::byte{1}});
  CHECK(trailing.has_value());
  if (!trailing) return;
  std::array<std::byte, tavern::page_size> raw{};
  CHECK(pread(store->file_descriptor, raw.data(), raw.size(),
              static_cast<off_t>(root * tavern::page_size)) ==
        static_cast<ssize_t>(raw.size()));
  bool child = false;
  for (std::size_t index = 0; index < tavern::directory_fanout; ++index)
    if (get64(raw, 21 + index * 8) != tavern::invalid_page_id) {
      put64(raw, 21 + index * 8, *trailing);
      child = true;
    }
  CHECK(child);
  if (!child) return;
  put64(raw, 12, checksum(raw));
  CHECK(pwrite(store->file_descriptor, raw.data(), raw.size(),
               static_cast<off_t>(root * tavern::page_size)) ==
        static_cast<ssize_t>(raw.size()));
  CHECK(!tavern::traverse_directory(*store, root, *trailing, callback, records)
             .has_value());
  CHECK(!tavern::traverse_directory(*store, root, *trailing - 1, callback,
                                    records)
             .has_value());
  CHECK(!tavern::directory_lookup(*store, root, *trailing, 1).has_value());
  CHECK(!tavern::directory_lookup(*store, root, *trailing - 1, 1).has_value());
  CHECK(tavern::close_page_store(*store).has_value());
  std::filesystem::remove(file);
}
}  // namespace
auto main() -> int {
  test_basics_and_cow();
  test_splits_and_random();
  test_traversal_extent();
  std::cout << checks << " checks, " << failures << " failure(s)\n";
  return failures == 0 ? 0 : 1;
}
