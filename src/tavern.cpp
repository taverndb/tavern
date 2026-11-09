#include "tavern.hpp"

#include <algorithm>
#include <new>

namespace tavern {
namespace {

struct Location {
  BlockId block = invalid_block_id;
  BlockId predecessor = invalid_block_id;
  std::uint16_t index = 0;
};

auto find_adjacency(const Tavern& tavern, PersonId id) -> const Adjacency* {
  const auto it = tavern.patrons.find(id);
  return it == tavern.patrons.end() ? nullptr : &it->second;
}

auto find_location(const BlockPool& pool, const AdjacencySet& set, PersonId id)
    -> Location {
  BlockId predecessor = invalid_block_id;
  BlockId current = set.head;
  while (current != invalid_block_id) {
    const AdjacencyBlock& block = pool.blocks[current];
    for (std::uint16_t index = 0; index < block.count; ++index) {
      if (block.values[index] == id) {
        return {current, predecessor, index};
      }
    }
    predecessor = current;
    current = block.next;
  }
  return {};
}

auto contains(const BlockPool& pool, const AdjacencySet& set, PersonId id)
    -> bool {
  return find_location(pool, set, id).block != invalid_block_id;
}

auto is_empty(const Adjacency& adjacency) -> bool {
  return adjacency.following.size == 0 && adjacency.followers.size == 0 &&
         adjacency.friends.size == 0;
}

void erase_if_empty(Tavern& tavern, PersonId id) {
  const auto it = tavern.patrons.find(id);
  if (it != tavern.patrons.end() && is_empty(it->second)) {
    tavern.patrons.erase(it);
  }
}

auto missing_entries(bool create_a, bool create_b) -> std::size_t {
  return static_cast<std::size_t>(create_a) +
         static_cast<std::size_t>(create_b);
}

void release_block(BlockPool& pool, BlockId id);

auto allocate_block(BlockPool& pool) -> std::expected<BlockId, Error> {
  if (pool.free_head != invalid_block_id) {
    const BlockId id = pool.free_head;
    AdjacencyBlock& block = pool.blocks[id];
    pool.free_head = block.free_next;
    block = AdjacencyBlock{};
    block.in_use = true;
    return id;
  }

  const std::size_t maximum_slots =
      std::min(pool.slot_limit, static_cast<std::size_t>(invalid_block_id));
  if (pool.blocks.size() >= maximum_slots) {
    return std::unexpected(Error::block_capacity_exhausted);
  }
  pool.blocks.emplace_back();
  const auto index = pool.blocks.size() - 1;
  const BlockId id = static_cast<BlockId>(index);
  pool.blocks[id].in_use = true;
  return id;
}

void rollback_creation(Tavern& tavern, bool inserted_a, bool inserted_b,
                       PersonId a, PersonId b, BlockId a_block,
                       BlockId b_block) {
  if (a_block != invalid_block_id) {
    release_block(tavern.block_pool, a_block);
  }
  if (b_block != invalid_block_id) {
    release_block(tavern.block_pool, b_block);
  }
  if (inserted_a) {
    tavern.patrons.erase(a);
  }
  if (inserted_b) {
    tavern.patrons.erase(b);
  }
}

void release_block(BlockPool& pool, BlockId id) {
  AdjacencyBlock& block = pool.blocks[id];
  block.count = 0;
  block.next = invalid_block_id;
  block.in_use = false;
  block.free_next = pool.free_head;
  pool.free_head = id;
}

auto needs_block(const BlockPool& pool, const AdjacencySet& set) -> bool {
  return set.tail == invalid_block_id ||
         pool.blocks[set.tail].count == adjacency_block_capacity;
}

void append_prepared(BlockPool& pool, AdjacencySet& set, PersonId id,
                     BlockId prepared_block) {
  if (prepared_block != invalid_block_id) {
    if (set.tail == invalid_block_id) {
      set.head = prepared_block;
    } else {
      pool.blocks[set.tail].next = prepared_block;
    }
    set.tail = prepared_block;
  }

  AdjacencyBlock& tail = pool.blocks[set.tail];
  tail.values[tail.count] = id;
  ++tail.count;
  ++set.size;
}

void remove_at(BlockPool& pool, AdjacencySet& set, Location location) {
  AdjacencyBlock& block = pool.blocks[location.block];
  --block.count;
  block.values[location.index] = block.values[block.count];
  --set.size;

  if (block.count != 0) {
    return;
  }

  const BlockId next = block.next;
  if (location.predecessor == invalid_block_id) {
    set.head = next;
  } else {
    pool.blocks[location.predecessor].next = next;
  }
  if (set.tail == location.block) {
    set.tail = location.predecessor;
  }
  release_block(pool, location.block);
}

auto create_relationship(Tavern& tavern, PersonId a, PersonId b,
                         AdjacencySet Adjacency::* a_set,
                         AdjacencySet Adjacency::* b_set)
    -> std::expected<void, Error> {
  const Adjacency* a_adjacency = find_adjacency(tavern, a);
  if (a_adjacency != nullptr &&
      contains(tavern.block_pool, a_adjacency->*a_set, b)) {
    return {};
  }

  const bool create_a = a_adjacency == nullptr;
  const bool create_b = find_adjacency(tavern, b) == nullptr;
  bool inserted_a = false;
  bool inserted_b = false;
  BlockId a_block = invalid_block_id;
  BlockId b_block = invalid_block_id;

  try {
    if (create_a || create_b) {
      tavern.patrons.reserve(tavern.patrons.size() +
                             missing_entries(create_a, create_b));
    }
    if (create_a) {
      inserted_a = tavern.patrons.try_emplace(a).second;
    }
    if (create_b) {
      inserted_b = tavern.patrons.try_emplace(b).second;
    }

    AdjacencySet& first = tavern.patrons.find(a)->second.*a_set;
    AdjacencySet& second = tavern.patrons.find(b)->second.*b_set;
    if (needs_block(tavern.block_pool, first)) {
      const auto allocated = allocate_block(tavern.block_pool);
      if (!allocated) {
        rollback_creation(tavern, inserted_a, inserted_b, a, b, a_block,
                          b_block);
        return std::unexpected(allocated.error());
      }
      a_block = *allocated;
    }
    if (needs_block(tavern.block_pool, second)) {
      const auto allocated = allocate_block(tavern.block_pool);
      if (!allocated) {
        rollback_creation(tavern, inserted_a, inserted_b, a, b, a_block,
                          b_block);
        return std::unexpected(allocated.error());
      }
      b_block = *allocated;
    }

    // Commit begins here: all links and writes are non-allocating.
    append_prepared(tavern.block_pool, first, b, a_block);
    append_prepared(tavern.block_pool, second, a, b_block);
    return {};
  } catch (const std::bad_alloc&) {
    rollback_creation(tavern, inserted_a, inserted_b, a, b, a_block, b_block);
    return std::unexpected(Error::out_of_memory);
  }
}

auto remove_relationship(Tavern& tavern, PersonId a, PersonId b,
                         AdjacencySet Adjacency::* a_set,
                         AdjacencySet Adjacency::* b_set)
    -> std::expected<void, Error> {
  const auto a_it = tavern.patrons.find(a);
  if (a_it == tavern.patrons.end()) {
    return {};
  }
  const Location a_location =
      find_location(tavern.block_pool, a_it->second.*a_set, b);
  if (a_location.block == invalid_block_id) {
    return {};
  }

  const auto b_it = tavern.patrons.find(b);
  const Location b_location =
      b_it == tavern.patrons.end()
          ? Location{}
          : find_location(tavern.block_pool, b_it->second.*b_set, a);

  remove_at(tavern.block_pool, a_it->second.*a_set, a_location);
  if (b_location.block != invalid_block_id) {
    remove_at(tavern.block_pool, b_it->second.*b_set, b_location);
  }

  erase_if_empty(tavern, a);
  erase_if_empty(tavern, b);
  return {};
}

template <typename Function>
void for_each_id(const BlockPool& pool, const AdjacencySet& set,
                 Function&& function) {
  BlockId current = set.head;
  while (current != invalid_block_id) {
    const AdjacencyBlock& block = pool.blocks[current];
    for (std::uint16_t index = 0; index < block.count; ++index) {
      function(block.values[index]);
    }
    current = block.next;
  }
}

auto collect(const Tavern& tavern, const AdjacencySet* set)
    -> std::expected<std::vector<PersonId>, Error> {
  try {
    if (set == nullptr) {
      return std::vector<PersonId>{};
    }
    std::vector<PersonId> result;
    result.reserve(set->size);
    for_each_id(tavern.block_pool, *set,
                [&result](PersonId id) -> void { result.push_back(id); });
    return result;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::out_of_memory);
  }
}

}  // namespace

auto follow(Tavern& tavern, PersonId a, PersonId b)
    -> std::expected<void, Error> {
  if (a == b) {
    return std::unexpected(Error::self_relationship);
  }
  return create_relationship(tavern, a, b, &Adjacency::following,
                             &Adjacency::followers);
}

auto unfollow(Tavern& tavern, PersonId a, PersonId b)
    -> std::expected<void, Error> {
  if (a == b) {
    return std::unexpected(Error::self_relationship);
  }
  return remove_relationship(tavern, a, b, &Adjacency::following,
                             &Adjacency::followers);
}

auto befriend(Tavern& tavern, PersonId a, PersonId b)
    -> std::expected<void, Error> {
  if (a == b) {
    return std::unexpected(Error::self_relationship);
  }
  return create_relationship(tavern, a, b, &Adjacency::friends,
                             &Adjacency::friends);
}

auto unfriend(Tavern& tavern, PersonId a, PersonId b)
    -> std::expected<void, Error> {
  if (a == b) {
    return std::unexpected(Error::self_relationship);
  }
  return remove_relationship(tavern, a, b, &Adjacency::friends,
                             &Adjacency::friends);
}

auto is_following(const Tavern& tavern, PersonId a, PersonId b) -> bool {
  const Adjacency* adjacency = find_adjacency(tavern, a);
  return adjacency != nullptr &&
         contains(tavern.block_pool, adjacency->following, b);
}

auto are_friends(const Tavern& tavern, PersonId a, PersonId b) -> bool {
  const Adjacency* adjacency = find_adjacency(tavern, a);
  return adjacency != nullptr &&
         contains(tavern.block_pool, adjacency->friends, b);
}

auto following(const Tavern& tavern, PersonId a)
    -> std::expected<std::vector<PersonId>, Error> {
  const Adjacency* adjacency = find_adjacency(tavern, a);
  return collect(tavern,
                 adjacency == nullptr ? nullptr : &adjacency->following);
}

auto followers(const Tavern& tavern, PersonId a)
    -> std::expected<std::vector<PersonId>, Error> {
  const Adjacency* adjacency = find_adjacency(tavern, a);
  return collect(tavern,
                 adjacency == nullptr ? nullptr : &adjacency->followers);
}

auto friends(const Tavern& tavern, PersonId a)
    -> std::expected<std::vector<PersonId>, Error> {
  const Adjacency* adjacency = find_adjacency(tavern, a);
  return collect(tavern, adjacency == nullptr ? nullptr : &adjacency->friends);
}

auto mutual_friends(const Tavern& tavern, PersonId a, PersonId b)
    -> std::expected<std::vector<PersonId>, Error> {
  try {
    const Adjacency* a_adjacency = find_adjacency(tavern, a);
    const Adjacency* b_adjacency = find_adjacency(tavern, b);
    if (a_adjacency == nullptr || b_adjacency == nullptr) {
      return std::vector<PersonId>{};
    }
    const AdjacencySet& first =
        a_adjacency->friends.size <= b_adjacency->friends.size
            ? a_adjacency->friends
            : b_adjacency->friends;
    const AdjacencySet& second =
        a_adjacency->friends.size <= b_adjacency->friends.size
            ? b_adjacency->friends
            : a_adjacency->friends;

    std::vector<PersonId> result;
    result.reserve(first.size);
    for_each_id(tavern.block_pool, first,
                [&tavern, &second, &result](PersonId id) -> void {
                  if (contains(tavern.block_pool, second, id)) {
                    result.push_back(id);
                  }
                });
    return result;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::out_of_memory);
  }
}

}  // namespace tavern
