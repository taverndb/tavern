#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <limits>
#include <tavern/database.hpp>
#include <unordered_map>
#include <vector>

namespace tavern {

using BlockId = std::uint32_t;

inline constexpr BlockId invalid_block_id = std::numeric_limits<BlockId>::max();
inline constexpr std::size_t adjacency_block_capacity = 16;

struct AdjacencySet {
  BlockId head = invalid_block_id;
  BlockId tail = invalid_block_id;
  std::size_t size = 0;
};

struct Adjacency {
  AdjacencySet following;
  AdjacencySet followers;
  AdjacencySet friends;
};

struct AdjacencyBlock {
  std::array<PersonId, adjacency_block_capacity> values{};
  std::uint16_t count = 0;
  BlockId next = invalid_block_id;
  BlockId free_next = invalid_block_id;
  bool in_use = false;
};

struct BlockPool {
  std::vector<AdjacencyBlock> blocks;
  BlockId free_head = invalid_block_id;
  // Number of allocatable slots. The invalid sentinel is deliberately excluded.
  std::size_t slot_limit = static_cast<std::size_t>(invalid_block_id);
};

struct Tavern {
  std::unordered_map<PersonId, Adjacency> patrons;
  BlockPool block_pool;
};

std::expected<void, Error> follow(Tavern& tavern, PersonId a, PersonId b);
std::expected<void, Error> unfollow(Tavern& tavern, PersonId a, PersonId b);
std::expected<void, Error> befriend(Tavern& tavern, PersonId a, PersonId b);
std::expected<void, Error> unfriend(Tavern& tavern, PersonId a, PersonId b);

bool is_following(const Tavern& tavern, PersonId a, PersonId b);
bool are_friends(const Tavern& tavern, PersonId a, PersonId b);

std::expected<std::vector<PersonId>, Error> following(const Tavern& tavern,
                                                      PersonId a);
std::expected<std::vector<PersonId>, Error> followers(const Tavern& tavern,
                                                      PersonId a);
std::expected<std::vector<PersonId>, Error> friends(const Tavern& tavern,
                                                    PersonId a);
std::expected<std::vector<PersonId>, Error> mutual_friends(const Tavern& tavern,
                                                           PersonId a,
                                                           PersonId b);

}  // namespace tavern
