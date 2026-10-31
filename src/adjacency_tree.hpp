#pragma once

#include <expected>
#include <functional>

#include "adjacency_pages.hpp"
#include "directory.hpp"

namespace tavern {

// Adjacency trees are append-oriented, bounded-fanout trees. A descriptor's
// height counts reference-page levels above leaves. Tree operations read only
// pages on the relevant traversal path or stream leaves; they need not load an
// entire relationship set into memory. Writes are copy-on-write, so returned
// descriptors must be installed in a newly written directory record before
// publication makes the new tree reachable.
inline constexpr std::size_t adjacency_tree_fanout = 16;

std::expected<AdjacencyDescriptor, Error> build_adjacency_tree(
    PageStore& store, std::span<const PersonId> ids);
std::expected<AdjacencyDescriptor, Error> adjacency_insert(
    PageStore& store, const AdjacencyDescriptor& descriptor, PersonId id);
std::expected<AdjacencyDescriptor, Error> adjacency_remove(
    PageStore& store, const AdjacencyDescriptor& descriptor, PersonId id);
std::expected<bool, Error> adjacency_contains(
    const PageStore& store, const AdjacencyDescriptor& descriptor, PersonId id);
std::expected<void, Error> traverse_adjacency_pages(
    const PageStore& store, const AdjacencyDescriptor& descriptor,
    PageId extent,
    const std::function<std::expected<void, Error>(PageId, PageType)>&
        callback);
std::expected<void, Error> for_each_adjacency(
    const PageStore& store, const AdjacencyDescriptor& descriptor,
    const std::function<std::expected<void, Error>(PersonId)>& callback);

}  // namespace tavern
