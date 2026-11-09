#include "persistent_relationship.hpp"

#include <unistd.h>

#include <filesystem>
#include <iostream>
auto main() -> int {
  const auto p = std::filesystem::temp_directory_path() /
                 ("tavern-rel-" +
                  std::to_string(static_cast<long long>(getpid())) + ".db");
  std::filesystem::remove(p);
  auto s = tavern::create_page_store(p);
  if (!s) return 1;
  tavern::PageId r = tavern::invalid_page_id;
  auto f = tavern::persistent_follow(*s, r, s->page_count, 1, 2);
  if (!f) return 1;
  r = *f;
  if (!tavern::persistent_is_following(*s, r, s->page_count, 1, 2).value())
    return 1;
  auto b = tavern::persistent_befriend(*s, r, s->page_count, 1, 2);
  if (!b) return 1;
  r = *b;
  if (!tavern::persistent_are_friends(*s, r, s->page_count, 2, 1).value())
    return 1;
  auto u = tavern::persistent_unfollow(*s, r, s->page_count, 1, 2);
  if (!u) return 1;
  r = *u;
  if (tavern::persistent_is_following(*s, r, s->page_count, 1, 2).value())
    return 1;
  auto x = tavern::persistent_unfriend(*s, r, s->page_count, 1, 2);
  if (!x) return 1;
  r = *x;
  if (tavern::persistent_are_friends(*s, r, s->page_count, 1, 2).value())
    return 1;
  tavern::close_page_store(*s);
  std::filesystem::remove(p);
  return 0;
}
