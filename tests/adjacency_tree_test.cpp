#include "adjacency_tree.hpp"

#include <unistd.h>

#include <filesystem>
#include <iostream>
#include <set>
#include <string>
namespace {
int n = 0, f = 0;
void c(bool x, const char* e, int l) {
  ++n;
  if (!x) {
    ++f;
    std::cerr << l << ":" << e << '\n';
  }
}
#define C(x) c(static_cast<bool>(x), #x, __LINE__)
}  // namespace
auto main() -> int {
  const auto p = std::filesystem::temp_directory_path() /
                 ("tavern-tree-" +
                  std::to_string(static_cast<long long>(getpid())) + ".db");
  std::filesystem::remove(p);
  auto s = tavern::create_page_store(p);
  C(s.has_value());
  if (!s) return 1;
  auto single = tavern::adjacency_insert(*s, {}, 77);
  C(single && single->count == 1 && single->height == 0);
  C(single && tavern::adjacency_insert(*s, *single, 77)->root == single->root);
  std::vector<tavern::PersonId> ids;
  ids.reserve(600);
  for (tavern::PersonId i = 0; i < 600; ++i) ids.push_back(i * 3 + 1);
  auto d = tavern::build_adjacency_tree(*s, ids);
  C(d.has_value());
  C(d && d->height == 1 && d->count == ids.size());
  C(d && tavern::adjacency_contains(*s, *d, 4).value());
  C(d && !tavern::adjacency_contains(*s, *d, 5).value());
  auto newer = d ? tavern::adjacency_insert(*s, *d, 999999)
                 : std::expected<tavern::AdjacencyDescriptor, tavern::Error>(
                       std::unexpected(tavern::Error::invalid_page));
  C(newer && newer->count == 601);
  C(d && newer && !tavern::adjacency_contains(*s, *d, 999999).value() &&
    tavern::adjacency_contains(*s, *newer, 999999).value());
  std::set<tavern::PersonId> got;
  C(d && tavern::for_each_adjacency(
             *s, *d,
             [&got](tavern::PersonId x) -> std::expected<void, tavern::Error> {
               got.insert(x);
               return std::expected<void, tavern::Error>{};
             })
             .has_value());
  C(got.size() == ids.size());
  C(!tavern::build_adjacency_tree(*s, std::vector<tavern::PersonId>{1, 1})
         .has_value());
  C(tavern::close_page_store(*s).has_value());
  auto r = tavern::open_page_store(p);
  C(r.has_value());
  C(r && d && tavern::adjacency_contains(*r, *d, 4).value());
  if (r) C(tavern::close_page_store(*r).has_value());
  std::filesystem::remove(p);
  std::cout << n << " checks, " << f << " failure(s)\n";
  return f == 0 ? 0 : 1;
}
