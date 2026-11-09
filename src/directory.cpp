#include "directory.hpp"

#include <algorithm>
#include <array>
#include <new>
#include <span>
#include <unordered_set>
#include <vector>

#include "page_store_internal.hpp"

namespace tavern {
namespace {

constexpr std::size_t node_payload_size = 1 + directory_fanout * 8;
constexpr std::size_t record_size = 8 + 3 * (8 + 4 + 8);
constexpr std::size_t leaf_header_size = 3;

void put16(std::span<std::byte> b, std::size_t o, std::uint16_t v) {
  b[o] = static_cast<std::byte>(v);
  b[o + 1] = static_cast<std::byte>(v >> 8U);
}
void put32(std::span<std::byte> b, std::size_t o, std::uint32_t v) {
  for (std::size_t i = 0; i < 4; ++i)
    b[o + i] = static_cast<std::byte>(v >> (i * 8U));
}
void put64(std::span<std::byte> b, std::size_t o, std::uint64_t v) {
  for (std::size_t i = 0; i < 8; ++i)
    b[o + i] = static_cast<std::byte>(v >> (i * 8U));
}
auto get16(std::span<const std::byte> b, std::size_t o) -> std::uint16_t {
  return static_cast<std::uint16_t>(std::to_integer<unsigned int>(b[o])) |
         static_cast<std::uint16_t>(std::to_integer<unsigned int>(b[o + 1])
                                    << 8U);
}
auto get32(std::span<const std::byte> b, std::size_t o) -> std::uint32_t {
  std::uint32_t v = 0;
  for (std::size_t i = 0; i < 4; ++i)
    v |= static_cast<std::uint32_t>(std::to_integer<unsigned int>(b[o + i]))
         << (i * 8U);
  return v;
}
auto get64(std::span<const std::byte> b, std::size_t o) -> std::uint64_t {
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < 8; ++i)
    v |= static_cast<std::uint64_t>(
             std::to_integer<unsigned long long>(b[o + i]))
         << (i * 8U);
  return v;
}

struct Node {
  std::uint8_t depth = 0;
  std::array<PageId, directory_fanout> child{};
};
struct Leaf {
  std::uint8_t depth = 0;
  std::vector<PatronRecord> records;
};

auto segment(std::uint64_t hash, std::uint8_t depth) -> std::uint8_t {
  return static_cast<std::uint8_t>((hash >> (depth * 4U)) & 0xFU);
}

void put_descriptor(std::span<std::byte> b, std::size_t o,
                    const AdjacencyDescriptor& d) {
  put64(b, o, d.root);
  put32(b, o + 8, d.height);
  put64(b, o + 12, d.count);
}
auto get_descriptor(std::span<const std::byte> b, std::size_t o)
    -> AdjacencyDescriptor {
  return {get64(b, o), get32(b, o + 8), get64(b, o + 12)};
}
void put_record(std::span<std::byte> b, std::size_t o, const PatronRecord& r) {
  put64(b, o, r.id);
  put_descriptor(b, o + 8, r.following);
  put_descriptor(b, o + 28, r.followers);
  put_descriptor(b, o + 48, r.friends);
}
auto get_record(std::span<const std::byte> b, std::size_t o) -> PatronRecord {
  return {get64(b, o), get_descriptor(b, o + 8), get_descriptor(b, o + 28),
          get_descriptor(b, o + 48)};
}

auto load_node(const PageStore& s, PageId id) -> std::expected<Node, Error> {
  const auto p = read_page(s, id);
  if (!p || p->type != PageType::directory_node ||
      p->payload.size() != node_payload_size)
    return std::unexpected(Error::invalid_directory);
  Node n{};
  n.depth = std::to_integer<std::uint8_t>(p->payload[0]);
  if (n.depth >= directory_max_depth)
    return std::unexpected(Error::invalid_directory);
  for (std::size_t i = 0; i < directory_fanout; ++i)
    n.child[i] = get64(p->payload, 1 + i * 8);
  return n;
}

auto load_leaf(const PageStore& s, PageId id) -> std::expected<Leaf, Error> {
  const auto p = read_page(s, id);
  if (!p || p->type != PageType::directory_leaf ||
      p->payload.size() < leaf_header_size)
    return std::unexpected(Error::invalid_directory);
  const std::uint8_t depth = std::to_integer<std::uint8_t>(p->payload[0]);
  const std::uint16_t count = get16(p->payload, 1);
  if (depth > directory_max_depth || count > directory_leaf_capacity ||
      p->payload.size() != leaf_header_size + count * record_size)
    return std::unexpected(Error::invalid_directory);
  try {
    Leaf leaf{depth, {}};
    leaf.records.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
      leaf.records.push_back(
          get_record(p->payload, leaf_header_size + i * record_size));
    return leaf;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::out_of_memory);
  }
}

auto write_leaf(PageStore& s, const Leaf& leaf)
    -> std::expected<PageId, Error> {
  if (leaf.depth > directory_max_depth ||
      leaf.records.size() > directory_leaf_capacity)
    return std::unexpected(Error::invalid_directory);
  try {
    std::vector<std::byte> bytes(leaf_header_size +
                                 leaf.records.size() * record_size);
    bytes[0] = static_cast<std::byte>(leaf.depth);
    put16(bytes, 1, static_cast<std::uint16_t>(leaf.records.size()));
    for (std::size_t i = 0; i < leaf.records.size(); ++i)
      put_record(bytes, leaf_header_size + i * record_size, leaf.records[i]);
    return storage_detail::allocate_page_for_active_session(
        s, PageType::directory_leaf, bytes);
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::out_of_memory);
  }
}

auto write_node(PageStore& s, const Node& node)
    -> std::expected<PageId, Error> {
  std::array<std::byte, node_payload_size> bytes{};
  bytes[0] = static_cast<std::byte>(node.depth);
  for (std::size_t i = 0; i < directory_fanout; ++i)
    put64(bytes, 1 + i * 8, node.child[i]);
  return storage_detail::allocate_page_for_active_session(
      s, PageType::directory_node, bytes);
}

auto build(PageStore& s, std::uint8_t depth,
           const std::vector<PatronRecord>& records)
    -> std::expected<PageId, Error> {
  if (records.size() <= directory_leaf_capacity)
    return write_leaf(s, {depth, records});
  if (depth >= directory_max_depth)
    return std::unexpected(Error::directory_capacity_exhausted);
  Node node{};
  node.depth = depth;
  node.child.fill(invalid_page_id);
  // A full leaf is repartitioned from this depth onward. Records with the same
  // remaining hash path stay together; exhausting all hash segments reports
  // capacity exhaustion rather than silently losing an adversarial collision.
  std::array<std::vector<PatronRecord>, directory_fanout> groups;
  try {
    for (const auto& record : records)
      groups[segment(directory_hash(record.id), depth)].push_back(record);
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::out_of_memory);
  }
  for (std::size_t i = 0; i < directory_fanout; ++i)
    if (!groups[i].empty()) {
      const auto child =
          build(s, static_cast<std::uint8_t>(depth + 1), groups[i]);
      if (!child) return std::unexpected(child.error());
      node.child[i] = *child;
    }
  return write_node(s, node);
}

auto modify(PageStore& s, PageId root, const PatronRecord& record, bool insert,
            bool remove) -> std::expected<PageId, Error> {
  if (root == invalid_page_id) {
    if (remove) return std::unexpected(Error::directory_missing);
    return write_leaf(s, {0, {record}});
  }
  const auto page = read_page(s, root);
  if (!page) return std::unexpected(Error::invalid_directory);
  if (page->type == PageType::directory_leaf) {
    // Every changed leaf and each ancestor on its search path receives a new
    // page. Siblings are referenced unchanged by the new ancestor, preserving
    // their visibility from earlier metadata generations.
    auto leaf = load_leaf(s, root);
    if (!leaf) return std::unexpected(leaf.error());
    const auto found =
        std::find_if(leaf->records.begin(), leaf->records.end(),
                     [&record](const PatronRecord& item) -> bool {
                       return item.id == record.id;
                     });
    if (remove) {
      if (found == leaf->records.end())
        return std::unexpected(Error::directory_missing);
      leaf->records.erase(found);
      return leaf->records.empty()
                 ? std::expected<PageId, Error>(invalid_page_id)
                 : write_leaf(s, *leaf);
    }
    if (insert && found != leaf->records.end())
      return std::unexpected(Error::directory_exists);
    if (!insert && found == leaf->records.end())
      return std::unexpected(Error::directory_missing);
    if (found != leaf->records.end())
      *found = record;
    else
      leaf->records.push_back(record);
    return build(s, leaf->depth, leaf->records);
  }
  if (page->type != PageType::directory_node)
    return std::unexpected(Error::invalid_directory);
  auto node = load_node(s, root);
  if (!node) return std::unexpected(node.error());
  const std::size_t child_index =
      segment(directory_hash(record.id), node->depth);
  const auto child =
      modify(s, node->child[child_index], record, insert, remove);
  if (!child) return std::unexpected(child.error());
  node->child[child_index] = *child;
  bool any = false;
  for (const PageId id : node->child) any = any || id != invalid_page_id;
  return any ? write_node(s, *node)
             : std::expected<PageId, Error>(invalid_page_id);
}

}  // namespace

auto directory_hash(PersonId id) -> std::uint64_t {
  id ^= id >> 30U;
  id *= 0xBF58476D1CE4E5B9ULL;
  id ^= id >> 27U;
  id *= 0x94D049BB133111EBULL;
  return id ^ (id >> 31U);
}

auto directory_lookup(const PageStore& store, PageId root, PageId extent,
                      PersonId id)
    -> std::expected<std::optional<PatronRecord>, Error> {
  if (extent < first_data_page || extent > store.page_count ||
      (root != invalid_page_id && (root < first_data_page || root >= extent))) {
    return std::unexpected(Error::invalid_directory);
  }
  PageId current = root;
  while (current != invalid_page_id) {
    if (current < first_data_page || current >= extent)
      return std::unexpected(Error::invalid_directory);
    const auto page = read_page(store, current);
    if (!page) return std::unexpected(Error::invalid_directory);
    if (page->type == PageType::directory_leaf) {
      const auto leaf = load_leaf(store, current);
      if (!leaf) return std::unexpected(leaf.error());
      const auto found = std::find_if(
          leaf->records.begin(), leaf->records.end(),
          [id](const PatronRecord& r) -> bool { return r.id == id; });
      return found == leaf->records.end() ? std::optional<PatronRecord>{}
                                          : std::optional<PatronRecord>{*found};
    }
    if (page->type != PageType::directory_node)
      return std::unexpected(Error::invalid_directory);
    const auto node = load_node(store, current);
    if (!node) return std::unexpected(node.error());
    current = node->child[segment(directory_hash(id), node->depth)];
  }
  return std::optional<PatronRecord>{};
}

auto traverse_directory(
    const PageStore& store, PageId root, PageId extent,
    const std::function<std::expected<void, Error>(PageId, PageType)>&
        page_callback,
    const std::function<std::expected<void, Error>(const PatronRecord&)>&
        record_callback) -> std::expected<void, Error> {
  try {
    if (extent < first_data_page || extent > store.page_count)
      return std::unexpected(Error::invalid_directory);
    // A well-formed directory is a tree. The visited set turns aliases and
    // cycles in corrupt on-disk pages into an error instead of unbounded work;
    // it also bounds traversal memory by the number of visited directory pages.
    std::unordered_set<PageId> visited;
    std::function<std::expected<void, Error>(PageId)> visit =
        [&](PageId page) -> std::expected<void, Error> {
      if (page < first_data_page || page >= extent ||
          !visited.insert(page).second)
        return std::unexpected(Error::invalid_directory);
      const auto raw = read_page(store, page);
      if (!raw) return std::unexpected(raw.error());
      const auto page_result = page_callback(page, raw->type);
      if (!page_result) return std::unexpected(page_result.error());
      if (raw->type == PageType::directory_leaf) {
        const auto leaf = load_leaf(store, page);
        if (!leaf) return std::unexpected(leaf.error());
        for (const PatronRecord& record : leaf->records) {
          const auto record_result = record_callback(record);
          if (!record_result) return std::unexpected(record_result.error());
        }
        return {};
      }
      if (raw->type != PageType::directory_node)
        return std::unexpected(Error::invalid_directory);
      const auto node = load_node(store, page);
      if (!node) return std::unexpected(node.error());
      for (const PageId child : node->child)
        if (child != invalid_page_id) {
          const auto child_result = visit(child);
          if (!child_result) return std::unexpected(child_result.error());
        }
      return {};
    };
    return root == invalid_page_id ? std::expected<void, Error>{} : visit(root);
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::out_of_memory);
  }
}

auto directory_insert(PageStore& store, PageId root, const PatronRecord& record)
    -> std::expected<PageId, Error> {
  return modify(store, root, record, true, false);
}
auto directory_update(PageStore& store, PageId root, const PatronRecord& record)
    -> std::expected<PageId, Error> {
  return modify(store, root, record, false, false);
}
auto directory_remove(PageStore& store, PageId root, PersonId id)
    -> std::expected<PageId, Error> {
  PatronRecord record{};
  record.id = id;
  return modify(store, root, record, false, true);
}

auto directory_outgoing_references(const PageStore& store, PageId page,
                                   PageId extent)
    -> std::expected<std::vector<PageId>, Error> {
  if (page < first_data_page || page >= extent)
    return std::unexpected(Error::invalid_directory);
  const auto raw = read_page(store, page);
  if (!raw) return std::unexpected(raw.error());
  try {
    std::vector<PageId> references;
    if (raw->type == PageType::directory_node) {
      const auto node = load_node(store, page);
      if (!node) return std::unexpected(node.error());
      for (const PageId child : node->child)
        if (child != invalid_page_id) references.push_back(child);
    } else if (raw->type == PageType::directory_leaf) {
      const auto leaf = load_leaf(store, page);
      if (!leaf) return std::unexpected(leaf.error());
      for (const PatronRecord& record : leaf->records)
        for (const AdjacencyDescriptor& descriptor :
             {record.following, record.followers, record.friends}) {
          const bool valid =
              descriptor.count == 0
                  ? descriptor.root == invalid_page_id && descriptor.height == 0
                  : descriptor.root != invalid_page_id &&
                        descriptor.root < extent;
          if (!valid) return std::unexpected(Error::invalid_directory);
          if (descriptor.count != 0) references.push_back(descriptor.root);
        }
    } else {
      return std::unexpected(Error::invalid_directory);
    }
    std::sort(references.begin(), references.end());
    references.erase(std::unique(references.begin(), references.end()),
                     references.end());
    return references;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::out_of_memory);
  }
}

}  // namespace tavern
