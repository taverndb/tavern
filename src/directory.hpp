#pragma once

#include <expected>
#include <functional>
#include <optional>

#include "page_store.hpp"

namespace tavern {

// A persistent adjacency tree is named by its root, number of reference levels,
// and logical member count. The empty representation is canonical: null root,
// zero height, and zero count. Directory records copy descriptors by value, so
// unmodified adjacency pages can be shared by multiple committed generations.
struct AdjacencyDescriptor {
  PageId root = invalid_page_id;
  std::uint32_t height = 0;
  std::uint64_t count = 0;
};

struct PatronRecord {
  PersonId id = 0;
  AdjacencyDescriptor following;
  AdjacencyDescriptor followers;
  AdjacencyDescriptor friends;
};

// The directory is a 16-way hash trie over PersonId hashes. A node consumes one
// four-bit hash segment; leaves retain complete records to resolve collisions.
// Depth is bounded by the 64-bit hash width. Lookup is proportional to visited
// trie levels plus a leaf scan, rather than an unconditional constant-time
// operation. Mutations rewrite only the searched path (and rebuild a leaf when
// it splits), leaving all other pages available to older metadata generations.
inline constexpr std::size_t directory_fanout = 16;
inline constexpr std::size_t directory_leaf_capacity = 59;
inline constexpr std::uint8_t directory_max_depth = 16;

std::uint64_t directory_hash(PersonId id);
std::expected<std::optional<PatronRecord>, Error> directory_lookup(
    const PageStore& store, PageId root, PageId extent, PersonId id);
std::expected<PageId, Error> directory_insert(PageStore& store, PageId root,
                                              const PatronRecord& record);
std::expected<PageId, Error> directory_update(PageStore& store, PageId root,
                                              const PatronRecord& record);
std::expected<PageId, Error> directory_remove(PageStore& store, PageId root,
                                              PersonId id);
std::expected<void, Error> traverse_directory(
    const PageStore& store, PageId root, PageId extent,
    const std::function<std::expected<void, Error>(PageId, PageType)>&
        page_callback,
    const std::function<std::expected<void, Error>(const PatronRecord&)>&
        record_callback);
std::expected<std::vector<PageId>, Error> directory_outgoing_references(
    const PageStore& store, PageId page, PageId extent);

}  // namespace tavern
