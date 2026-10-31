#include "page_store.hpp"

#include <unistd.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const char* expression, int line) {
  ++checks;
  if (!condition) {
    ++failures;
    std::cerr << line << ": check failed: " << expression << '\n';
  }
}

#define CHECK(expression) \
  check(static_cast<bool>(expression), #expression, __LINE__)

auto test_path(const char* suffix) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         (std::string("tavern-page-store-") +
          std::to_string(static_cast<long long>(getpid())) + suffix);
}

void test_create_append_reopen() {
  const auto path = test_path("-create.db");
  std::filesystem::remove(path);
  auto created = tavern::create_page_store(path);
  CHECK(created.has_value());
  if (!created) {
    return;
  }
  CHECK(created->page_count == tavern::first_data_page);
  const auto metadata_a =
      tavern::read_metadata(*created, tavern::metadata_slot_a);
  const auto metadata_b =
      tavern::read_metadata(*created, tavern::metadata_slot_b);
  CHECK(metadata_a.has_value());
  CHECK(metadata_b.has_value());
  if (metadata_a) {
    CHECK(metadata_a->committed_page_count == tavern::first_data_page);
    CHECK(metadata_a->root == tavern::invalid_page_id);
  }

  const std::array payload{std::byte{0x01}, std::byte{0xFE}, std::byte{0x7A}};
  const auto id =
      tavern::append_page(*created, tavern::PageType::data, payload);
  CHECK(id.has_value());
  CHECK(id && *id == tavern::first_data_page);
  CHECK(tavern::sync_page_store(*created).has_value());
  CHECK(tavern::close_page_store(*created).has_value());

  auto reopened = tavern::open_page_store(path);
  CHECK(reopened.has_value());
  if (reopened) {
    const auto page = tavern::read_page(*reopened, tavern::first_data_page);
    CHECK(page.has_value());
    if (page) {
      CHECK(page->type == tavern::PageType::data);
      CHECK(page->payload ==
            std::vector<std::byte>(payload.begin(), payload.end()));
    }
    CHECK(tavern::close_page_store(*reopened).has_value());
  }
  std::filesystem::remove(path);
}

void test_invalid_ids_corruption_and_truncation() {
  const auto path = test_path("-invalid.db");
  std::filesystem::remove(path);
  auto store = tavern::create_page_store(path);
  CHECK(store.has_value());
  if (!store) {
    return;
  }
  CHECK(!tavern::read_page(*store, tavern::invalid_page_id).has_value());
  CHECK(!tavern::read_metadata(*store, tavern::first_data_page).has_value());
  CHECK(!tavern::append_page(
             *store, tavern::PageType::data,
             std::vector<std::byte>(tavern::page_size, std::byte{}))
             .has_value());

  const auto id = tavern::append_page(*store, tavern::PageType::data, {});
  CHECK(id.has_value());
  if (id) {
    const std::byte corrupt{0xAA};
    CHECK(pwrite(store->file_descriptor, &corrupt, 1,
                 static_cast<off_t>(*id * tavern::page_size + 25)) == 1);
    CHECK(!tavern::read_page(*store, *id).has_value());
  }
  const std::byte metadata_corrupt{0x55};
  CHECK(pwrite(store->file_descriptor, &metadata_corrupt, 1, 40) == 1);
  CHECK(!tavern::read_metadata(*store, tavern::metadata_slot_a).has_value());
  CHECK(tavern::close_page_store(*store).has_value());

  const auto truncated = test_path("-truncated.db");
  std::filesystem::remove(truncated);
  std::FILE* file = std::fopen(truncated.c_str(), "wb");
  CHECK(file != nullptr);
  if (file != nullptr) {
    const std::array<char, 3> bytes{};
    CHECK(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
    CHECK(std::fclose(file) == 0);
  }
  CHECK(!tavern::open_page_store(truncated).has_value());
  std::filesystem::remove(path);
  std::filesystem::remove(truncated);
}

void test_append_failure_hook() {
  const auto path = test_path("-append-failure.db");
  std::filesystem::remove(path);
  auto store = tavern::create_page_store(path);
  CHECK(store.has_value());
  if (!store) return;
  store->fail_append_after_for_test = 1;
  const auto first = tavern::append_page(*store, tavern::PageType::data, {});
  const auto failed = tavern::append_page(*store, tavern::PageType::data, {});
  CHECK(first.has_value());
  CHECK(!failed.has_value());
  CHECK(first && tavern::read_page(*store, *first).has_value());
  const auto later = tavern::append_page(*store, tavern::PageType::data, {});
  CHECK(later.has_value());
  CHECK(first && later && *later == *first + 1);
  CHECK(tavern::close_page_store(*store).has_value());
  std::filesystem::remove(path);
}

}  // namespace

auto main() -> int {
  test_create_append_reopen();
  test_invalid_ids_corruption_and_truncation();
  test_append_failure_hook();
  std::cout << checks << " checks, " << failures << " failure(s)\n";
  return failures == 0 ? 0 : 1;
}
