#include <unistd.h>

#include <filesystem>
#include <iostream>
#include <random>

#include "database_internal.hpp"
namespace {
int checks = 0, failures = 0;
void check(bool v, const char* e, int l) {
  ++checks;
  if (!v) {
    ++failures;
    std::cerr << l << ":" << e << '\n';
  }
}
#define C(x) check(static_cast<bool>(x), #x, __LINE__)
auto path() -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         ("tavern-db-" + std::to_string(static_cast<long long>(getpid())) +
          ".db");
}
}  // namespace
auto main() -> int {
  const auto p = path();
  std::filesystem::remove(p);
  auto d = tavern::create_database(p);
  C(d.has_value());
  if (!d) return 1;
  C(database_impl(*d).commit.generation == 0);
  C(tavern::follow(*d, 1, 2).has_value());
  C(database_impl(*d).commit.generation == 1);
  const auto slot1 = tavern::read_metadata(
      database_impl(*d).store, database_impl(*d).commit.active_slot);
  C(slot1 && slot1->root == database_impl(*d).commit.root &&
    slot1->generation == 1);
  C(tavern::follow(*d, 1, 2).has_value());
  C(database_impl(*d).commit.generation == 1);
  C(tavern::befriend(*d, 1, 2).has_value());
  C(tavern::are_friends(*d, 2, 1).value());
  C(tavern::unfollow(*d, 1, 2).has_value());
  C(!tavern::is_following(*d, 1, 2).value());
  C(tavern::are_friends(*d, 1, 2).value());
  C(tavern::unfriend(*d, 1, 2).has_value());
  C(!tavern::are_friends(*d, 1, 2).value());
  tavern::Tavern memory;
  std::mt19937_64 rng(12345);
  for (int i = 0; i < 2000; ++i) {
    auto a = static_cast<tavern::PersonId>(rng() % 8),
         b = static_cast<tavern::PersonId>(rng() % 8), op = rng() % 4;
    if (op == 0) {
      C(tavern::follow(*d, a, b).has_value() ==
        tavern::follow(memory, a, b).has_value());
    } else if (op == 1) {
      C(tavern::unfollow(*d, a, b).has_value() ==
        tavern::unfollow(memory, a, b).has_value());
    } else if (op == 2) {
      C(tavern::befriend(*d, a, b).has_value() ==
        tavern::befriend(memory, a, b).has_value());
    } else {
      C(tavern::unfriend(*d, a, b).has_value() ==
        tavern::unfriend(memory, a, b).has_value());
    }
    if (i % 50 == 0)
      for (tavern::PersonId x = 0; x < 8; ++x)
        for (tavern::PersonId y = 0; y < 8; ++y) {
          C(tavern::is_following(*d, x, y).value() ==
            tavern::is_following(memory, x, y));
          C(tavern::are_friends(*d, x, y).value() ==
            tavern::are_friends(memory, x, y));
        }
  }
  C(!tavern::follow(*d, 3, 3).has_value());
  const auto root = database_impl(*d).commit.root;
  database_impl(*d).store.fail_data_sync_for_test = true;
  C(!tavern::follow(*d, 20, 21).has_value());
  C(database_impl(*d).commit.root == root);
  database_impl(*d).store.fail_data_sync_for_test = false;
  database_impl(*d).store.fail_metadata_sync_for_test = true;
  C(!tavern::follow(*d, 20, 21).has_value());
  C(database_impl(*d).commit.recovery_required);
  C(!tavern::is_following(*d, 20, 21).has_value());
  C(!tavern::follow(*d, 22, 23).has_value());
  C(tavern::close_database(*d).has_value());
  std::filesystem::remove(p);
  std::cout << checks << " checks, " << failures << " failure(s)\n";
  return failures == 0 ? 0 : 1;
}
