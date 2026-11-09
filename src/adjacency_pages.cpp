#include "adjacency_pages.hpp"

#include <array>
#include <new>

#include "page_store_internal.hpp"

namespace tavern {
namespace {
constexpr std::size_t payload_size = 2 + adjacency_page_capacity * 8;
void put16(std::span<std::byte> b, std::uint16_t v) {
  b[0] = static_cast<std::byte>(v);
  b[1] = static_cast<std::byte>(v >> 8U);
}
auto get16(std::span<const std::byte> b) -> std::uint16_t {
  return static_cast<std::uint16_t>(std::to_integer<unsigned int>(b[0])) |
         static_cast<std::uint16_t>(std::to_integer<unsigned int>(b[1]) << 8U);
}
void put64(std::span<std::byte> b, std::size_t o, std::uint64_t v) {
  for (std::size_t i = 0; i < 8; ++i)
    b[o + i] = static_cast<std::byte>(v >> (i * 8U));
}
auto get64(std::span<const std::byte> b, std::size_t o) -> std::uint64_t {
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < 8; ++i)
    v |= static_cast<std::uint64_t>(
             std::to_integer<unsigned long long>(b[o + i]))
         << (i * 8U);
  return v;
}

auto write_values(PageStore& store, PageType type,
                  std::span<const std::uint64_t> values)
    -> std::expected<PageId, Error> {
  if (values.size() > adjacency_page_capacity)
    return std::unexpected(Error::invalid_page);
  std::array<std::byte, payload_size> payload{};
  put16(payload, static_cast<std::uint16_t>(values.size()));
  for (std::size_t i = 0; i < values.size(); ++i)
    put64(payload, 2 + i * 8, values[i]);
  return storage_detail::allocate_page_for_active_session(store, type, payload);
}

auto read_values(const PageStore& store, PageId id, PageType type)
    -> std::expected<std::vector<std::uint64_t>, Error> {
  const auto page = read_page(store, id);
  if (!page || page->type != type || page->payload.size() != payload_size)
    return std::unexpected(Error::invalid_page);
  const std::uint16_t count = get16(page->payload);
  if (count > adjacency_page_capacity)
    return std::unexpected(Error::invalid_page);
  try {
    std::vector<std::uint64_t> result;
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
      result.push_back(get64(page->payload, 2 + i * 8));
    return result;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::out_of_memory);
  }
}
}  // namespace

auto write_adjacency_leaf(PageStore& store, std::span<const PersonId> ids)
    -> std::expected<PageId, Error> {
  return write_values(store, PageType::adjacency_leaf, ids);
}
auto read_adjacency_leaf(const PageStore& store, PageId id)
    -> std::expected<std::vector<PersonId>, Error> {
  return read_values(store, id, PageType::adjacency_leaf);
}
auto write_adjacency_reference(PageStore& store,
                               std::span<const PageId> children)
    -> std::expected<PageId, Error> {
  for (const PageId child : children)
    if (child == invalid_page_id) return std::unexpected(Error::invalid_page);
  return write_values(store, PageType::adjacency_reference, children);
}
auto read_adjacency_reference(const PageStore& store, PageId id)
    -> std::expected<std::vector<PageId>, Error> {
  const auto values = read_values(store, id, PageType::adjacency_reference);
  if (!values) return std::unexpected(values.error());
  for (const PageId child : *values)
    if (child == invalid_page_id) return std::unexpected(Error::invalid_page);
  return *values;
}
}  // namespace tavern
