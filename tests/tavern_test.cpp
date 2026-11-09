#include "tavern.hpp"

#include <algorithm>
#include <iostream>
#include <random>
#include <set>
#include <utility>
#include <vector>

namespace {

using tavern::AdjacencySet;
using tavern::BlockId;
using tavern::Error;
using tavern::PersonId;
using tavern::Tavern;
using IdSet = std::set<PersonId>;

int checks = 0;
int failures = 0;

void check(bool condition, const char* expression, const char* file, int line) {
  ++checks;
  if (!condition) {
    ++failures;
    std::cerr << file << ':' << line << ": check failed: " << expression
              << '\n';
  }
}

#define CHECK(expression) \
  check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

auto as_set(const std::vector<PersonId>& values) -> IdSet {
  return {values.begin(), values.end()};
}

auto query_set(std::expected<std::vector<PersonId>, Error> result) -> IdSet {
  CHECK(result.has_value());
  return result ? as_set(*result) : IdSet{};
}

auto block_count(const Tavern& tavern, const AdjacencySet& set) -> std::size_t {
  std::size_t count = 0;
  for (BlockId id = set.head; id != tavern::invalid_block_id;
       id = tavern.block_pool.blocks[id].next) {
    ++count;
  }
  return count;
}

void check_set_integrity(const Tavern& tavern, const AdjacencySet& set) {
  std::size_t size = 0;
  BlockId previous = tavern::invalid_block_id;
  std::set<PersonId> seen;
  for (BlockId id = set.head; id != tavern::invalid_block_id;
       id = tavern.block_pool.blocks[id].next) {
    CHECK(id < tavern.block_pool.blocks.size());
    const auto& block = tavern.block_pool.blocks[id];
    CHECK(block.in_use);
    CHECK(block.count > 0);
    CHECK(block.count <= tavern::adjacency_block_capacity);
    for (std::uint16_t index = 0; index < block.count; ++index) {
      CHECK(seen.insert(block.values[index]).second);
    }
    size += block.count;
    previous = id;
  }
  CHECK(size == set.size);
  CHECK((set.size == 0) == (set.head == tavern::invalid_block_id));
  CHECK((set.size == 0) == (set.tail == tavern::invalid_block_id));
  if (set.size != 0) {
    CHECK(previous == set.tail);
  }
}

void check_invariants(const Tavern& tavern) {
  for (const auto& [id, adjacency] : tavern.patrons) {
    check_set_integrity(tavern, adjacency.following);
    check_set_integrity(tavern, adjacency.followers);
    check_set_integrity(tavern, adjacency.friends);

    for (const PersonId other : query_set(tavern::following(tavern, id))) {
      CHECK(other != id);
      CHECK(query_set(tavern::followers(tavern, other)).contains(id));
    }
    for (const PersonId other : query_set(tavern::followers(tavern, id))) {
      CHECK(other != id);
      CHECK(query_set(tavern::following(tavern, other)).contains(id));
    }
    for (const PersonId other : query_set(tavern::friends(tavern, id))) {
      CHECK(other != id);
      CHECK(query_set(tavern::friends(tavern, other)).contains(id));
    }
  }
}

void test_domain_semantics() {
  Tavern tavern;
  CHECK(tavern::follow(tavern, 1, 2).has_value());
  CHECK(tavern::follow(tavern, 1, 2).has_value());
  CHECK(tavern::is_following(tavern, 1, 2));
  CHECK(!tavern::is_following(tavern, 2, 1));
  CHECK((query_set(tavern::following(tavern, 1)) == IdSet{2}));
  CHECK((query_set(tavern::followers(tavern, 2)) == IdSet{1}));

  CHECK(tavern::befriend(tavern, 1, 2).has_value());
  CHECK(tavern::are_friends(tavern, 1, 2));
  CHECK(tavern::are_friends(tavern, 2, 1));
  CHECK(tavern::unfollow(tavern, 1, 2).has_value());
  CHECK(!tavern::is_following(tavern, 1, 2));
  CHECK(tavern::are_friends(tavern, 1, 2));
  CHECK(tavern::unfriend(tavern, 2, 1).has_value());
  CHECK(tavern.patrons.empty());

  for (const auto operation :
       {tavern::follow, tavern::unfollow, tavern::befriend, tavern::unfriend}) {
    const auto result = operation(tavern, 9, 9);
    CHECK(!result.has_value());
    CHECK(result.error() == Error::self_relationship);
    CHECK(tavern.patrons.empty());
  }
  CHECK(query_set(tavern::following(tavern, 100)).empty());
  CHECK(query_set(tavern::followers(tavern, 100)).empty());
  CHECK(query_set(tavern::friends(tavern, 100)).empty());
  CHECK(query_set(tavern::mutual_friends(tavern, 100, 101)).empty());
}

void test_blocks_and_deletions() {
  Tavern tavern;
  constexpr PersonId source = 1;
  constexpr PersonId first_target = 100;
  const PersonId total =
      static_cast<PersonId>(tavern::adjacency_block_capacity * 2 + 3);

  for (PersonId offset = 0; offset < total; ++offset) {
    CHECK(tavern::follow(tavern, source, first_target + offset).has_value());
  }
  const auto source_it = tavern.patrons.find(source);
  CHECK(source_it != tavern.patrons.end());
  if (source_it != tavern.patrons.end()) {
    CHECK(source_it->second.following.size == total);
    CHECK(block_count(tavern, source_it->second.following) == 3);
  }
  check_invariants(tavern);

  CHECK(tavern::unfollow(tavern, source, first_target).has_value());
  CHECK(tavern::unfollow(tavern, source,
                         first_target + tavern::adjacency_block_capacity / 2)
            .has_value());
  CHECK(tavern::unfollow(tavern, source, first_target + total - 1).has_value());
  CHECK(!tavern::is_following(tavern, source, first_target));
  CHECK(!tavern::is_following(
      tavern, source, first_target + tavern::adjacency_block_capacity / 2));
  CHECK(!tavern::is_following(tavern, source, first_target + total - 1));

  for (PersonId offset = 0; offset < total; ++offset) {
    CHECK(tavern::unfollow(tavern, source, first_target + offset).has_value());
  }
  CHECK(!tavern.patrons.contains(source));
  CHECK(tavern.patrons.empty());
  CHECK(tavern.block_pool.free_head != tavern::invalid_block_id);
  check_invariants(tavern);

  // Reuse released slots without changing external semantics.
  CHECK(tavern::follow(tavern, 2, 3).has_value());
  CHECK(tavern::follow(tavern, 2, 3).has_value());
  CHECK((query_set(tavern::following(tavern, 2)) == IdSet{3}));
  check_invariants(tavern);
}

void test_block_capacity_exhaustion() {
  Tavern partial;
  partial.block_pool.slot_limit = 1;
  const auto partial_result = tavern::follow(partial, 1, 2);
  CHECK(!partial_result.has_value());
  CHECK(partial_result.error() == Error::block_capacity_exhausted);
  CHECK(partial.patrons.empty());
  CHECK(partial.block_pool.blocks.size() == 1);
  CHECK(partial.block_pool.free_head != tavern::invalid_block_id);

  Tavern tavern;
  tavern.block_pool.slot_limit = 2;
  CHECK(tavern::follow(tavern, 1, 2).has_value());
  const auto exhausted = tavern::follow(tavern, 1, 3);
  CHECK(!exhausted.has_value());
  CHECK(exhausted.error() == Error::block_capacity_exhausted);
  CHECK(tavern::is_following(tavern, 1, 2));
  CHECK(!tavern::is_following(tavern, 1, 3));
  CHECK(!tavern.patrons.contains(3));

  CHECK(tavern::unfollow(tavern, 1, 2).has_value());
  CHECK(tavern.patrons.empty());
  CHECK(tavern::follow(tavern, 3, 4).has_value());
  CHECK(tavern::is_following(tavern, 3, 4));
  CHECK(tavern.block_pool.blocks.size() == 2);
  check_invariants(tavern);
}

void test_mutual_friends() {
  Tavern tavern;
  for (const PersonId id : {2U, 3U, 4U}) {
    CHECK(tavern::befriend(tavern, 1, id).has_value());
  }
  for (const PersonId id : {3U, 4U, 5U}) {
    CHECK(tavern::befriend(tavern, 2, id).has_value());
  }
  CHECK((query_set(tavern::mutual_friends(tavern, 1, 2)) == IdSet{3, 4}));
  CHECK(tavern::unfriend(tavern, 2, 3).has_value());
  CHECK((query_set(tavern::mutual_friends(tavern, 1, 2)) == IdSet{4}));
  check_invariants(tavern);
}

struct Reference {
  std::set<std::pair<PersonId, PersonId>> follows;
  std::set<std::pair<PersonId, PersonId>> friendships;
};

auto ordered(PersonId a, PersonId b) -> std::pair<PersonId, PersonId> {
  return a < b ? std::pair<PersonId, PersonId>{a, b}
               : std::pair<PersonId, PersonId>{b, a};
}

void compare_reference(const Tavern& tavern, const Reference& reference,
                       PersonId max_id) {
  std::vector<IdSet> expected_following(static_cast<std::size_t>(max_id) + 1);
  std::vector<IdSet> expected_followers(static_cast<std::size_t>(max_id) + 1);
  std::vector<IdSet> expected_friends(static_cast<std::size_t>(max_id) + 1);
  for (const auto& [a, b] : reference.follows) {
    expected_following[a].insert(b);
    expected_followers[b].insert(a);
  }
  for (const auto& [a, b] : reference.friendships) {
    expected_friends[a].insert(b);
    expected_friends[b].insert(a);
  }
  for (PersonId a = 0; a <= max_id; ++a) {
    CHECK(query_set(tavern::following(tavern, a)) == expected_following[a]);
    CHECK(query_set(tavern::followers(tavern, a)) == expected_followers[a]);
    CHECK(query_set(tavern::friends(tavern, a)) == expected_friends[a]);
    for (PersonId b = 0; b <= max_id; ++b) {
      CHECK(tavern::is_following(tavern, a, b) ==
            reference.follows.contains({a, b}));
      CHECK(tavern::are_friends(tavern, a, b) ==
            reference.friendships.contains(ordered(a, b)));
    }
  }
}

void test_randomized_reference() {
  constexpr PersonId max_id = 15;
  Tavern tavern;
  Reference reference;
  std::mt19937_64 random(0x5EED1234ULL);
  for (int step = 0; step < 1200; ++step) {
    const PersonId a = random() % (max_id + 1);
    const PersonId b = random() % (max_id + 1);
    const bool self = a == b;
    const auto operation = random() % 4;
    std::expected<void, Error> result;
    if (operation == 0) {
      result = tavern::follow(tavern, a, b);
      if (!self) {
        reference.follows.insert({a, b});
      }
    } else if (operation == 1) {
      result = tavern::unfollow(tavern, a, b);
      if (!self) {
        reference.follows.erase({a, b});
      }
    } else if (operation == 2) {
      result = tavern::befriend(tavern, a, b);
      if (!self) {
        reference.friendships.insert(ordered(a, b));
      }
    } else {
      result = tavern::unfriend(tavern, a, b);
      if (!self) {
        reference.friendships.erase(ordered(a, b));
      }
    }
    CHECK(result.has_value() == !self);
    if (self) {
      CHECK(result.error() == Error::self_relationship);
    }
    check_invariants(tavern);
    compare_reference(tavern, reference, max_id);
  }
}

}  // namespace

auto main() -> int {
  test_domain_semantics();
  test_blocks_and_deletions();
  test_block_capacity_exhaustion();
  test_mutual_friends();
  test_randomized_reference();
  std::cout << checks << " checks, " << failures << " failure(s)\n";
  return failures == 0 ? 0 : 1;
}
