#pragma once

#include "page_store.hpp"

namespace tavern::storage_detail {

// The caller must hold an unconsumed reservation from a fresh FreePagePool for
// destination, and destination must be unreachable from all protected roots.
// This operation neither commits nor publishes the supplied page.
std::expected<void, Error> overwrite_reserved_unreachable_page(
    PageStore& store, PageId destination,
    std::span<const std::byte> encoded_page);
std::expected<PageId, Error> append_encoded_page(
    PageStore& store, std::span<const std::byte> encoded_page);
std::expected<PageId, Error> allocate_page_for_active_session(
    PageStore& store, PageType type, std::span<const std::byte> payload);

}  // namespace tavern::storage_detail
