#include "adjacency_pages.hpp"

#include <unistd.h>

#include <filesystem>
#include <iostream>
#include <string>

namespace {
int checks = 0;
int failures = 0;
void check(bool v, const char* e, int l) {
  ++checks;
  if (!v) {
    ++failures;
    std::cerr << l << ": " << e << '\n';
  }
}
#define CHECK(x) check(static_cast<bool>(x), #x, __LINE__)
auto path() -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         ("tavern-adj-" + std::to_string(static_cast<long long>(getpid())) +
          ".db");
}
}  // namespace
auto main() -> int {
  const auto file = path();
  std::filesystem::remove(file);
  auto store = tavern::create_page_store(file);
  CHECK(store.has_value());
  if (!store) return 1;
  const std::vector<tavern::PersonId> ids{1, 9, 42};
  const auto leaf = tavern::write_adjacency_leaf(*store, ids);
  CHECK(leaf.has_value());
  CHECK(leaf && tavern::read_adjacency_leaf(*store, *leaf).value() == ids);
  std::vector<tavern::PageId> full(tavern::adjacency_page_capacity);
  for (std::size_t i = 0; i < full.size(); ++i)
    full[i] = static_cast<tavern::PageId>(i + 2);
  const auto reference = tavern::write_adjacency_reference(*store, full);
  CHECK(reference.has_value());
  CHECK(reference &&
        tavern::read_adjacency_reference(*store, *reference).value() == full);
  full.push_back(99);
  CHECK(!tavern::write_adjacency_reference(*store, full).has_value());
  const std::vector<tavern::PageId> invalid{tavern::invalid_page_id};
  CHECK(!tavern::write_adjacency_reference(*store, invalid).has_value());
  CHECK(leaf && !tavern::read_adjacency_reference(*store, *leaf).has_value());
  CHECK(tavern::close_page_store(*store).has_value());
  std::filesystem::remove(file);
  std::cout << checks << " checks, " << failures << " failure(s)\n";
  return failures == 0 ? 0 : 1;
}
