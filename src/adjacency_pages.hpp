#pragma once

#include <expected>
#include <span>
#include <vector>

#include "page_store.hpp"

namespace tavern {

// Leaves contain relationship PersonIds; reference pages contain child PageIds.
// Both have a fixed payload size and an explicit entry count, allowing a tree
// page to be decoded independently. Reference pages never encode a null child.
// The capacity follows the shared fixed-page format, not an in-memory layout.
inline constexpr std::size_t adjacency_page_capacity =
    (page_size - 20 - 2) / sizeof(PersonId);

std::expected<PageId, Error> write_adjacency_leaf(
    PageStore& store, std::span<const PersonId> ids);
std::expected<std::vector<PersonId>, Error> read_adjacency_leaf(
    const PageStore& store, PageId id);
std::expected<PageId, Error> write_adjacency_reference(
    PageStore& store, std::span<const PageId> children);
std::expected<std::vector<PageId>, Error> read_adjacency_reference(
    const PageStore& store, PageId id);

}  // namespace tavern
