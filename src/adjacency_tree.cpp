#include "adjacency_tree.hpp"

#include <algorithm>
#include <new>
#include <unordered_set>

namespace tavern {
namespace {

auto traverse(
    const PageStore& store, PageId page, std::uint32_t height,
    const std::function<std::expected<void, Error>(PersonId)>& callback,
    std::uint64_t& seen) -> std::expected<void, Error> {
  if (page == invalid_page_id) return std::unexpected(Error::invalid_page);
  if (height == 0) {
    const auto leaf = read_adjacency_leaf(store, page);
    if (!leaf) return std::unexpected(leaf.error());
    for (const PersonId id : *leaf) {
      if (seen == UINT64_MAX) return std::unexpected(Error::page_limit);
      ++seen;
      const auto result = callback(id);
      if (!result) return std::unexpected(result.error());
    }
    return {};
  }
  const auto children = read_adjacency_reference(store, page);
  if (!children) return std::unexpected(children.error());
  for (const PageId child : *children) {
    const auto result = traverse(store, child, height - 1, callback, seen);
    if (!result) return std::unexpected(result.error());
  }
  return {};
}
}  // namespace

auto build_adjacency_tree(PageStore& store, std::span<const PersonId> ids)
    -> std::expected<AdjacencyDescriptor, Error> {
  if (ids.empty()) return AdjacencyDescriptor{};
  try {
    std::unordered_set<PersonId> unique;
    unique.reserve(ids.size());
    for (const PersonId id : ids)
      if (!unique.insert(id).second)
        return std::unexpected(Error::invalid_page);
    // Constructing bottom-up keeps only one level of PageIds in memory. The
    // produced tree preserves input order within leaves, but callers must not
    // infer a sorted order from this representation.
    std::vector<PageId> level;
    for (std::size_t offset = 0; offset < ids.size();
         offset += adjacency_page_capacity) {
      const std::size_t length =
          std::min(adjacency_page_capacity, ids.size() - offset);
      const auto page =
          write_adjacency_leaf(store, ids.subspan(offset, length));
      if (!page) return std::unexpected(page.error());
      level.push_back(*page);
    }
    std::uint32_t height = 0;
    while (level.size() > 1) {
      if (height == UINT32_MAX) return std::unexpected(Error::page_limit);
      std::vector<PageId> next;
      for (std::size_t offset = 0; offset < level.size();
           offset += adjacency_tree_fanout) {
        const std::size_t length =
            std::min(adjacency_tree_fanout, level.size() - offset);
        const auto page = write_adjacency_reference(
            store, std::span<const PageId>(level).subspan(offset, length));
        if (!page) return std::unexpected(page.error());
        next.push_back(*page);
      }
      level = std::move(next);
      ++height;
    }
    return AdjacencyDescriptor{level.front(), height,
                               static_cast<std::uint64_t>(ids.size())};
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::out_of_memory);
  }
}

auto adjacency_insert(PageStore& store, const AdjacencyDescriptor& descriptor,
                      PersonId id)
    -> std::expected<AdjacencyDescriptor, Error> {
  const auto present = adjacency_contains(store, descriptor, id);
  if (!present) return std::unexpected(present.error());
  if (*present) return descriptor;
  if (descriptor.count == UINT64_MAX) return std::unexpected(Error::page_limit);
  if (descriptor.count == 0) {
    const std::array<PersonId, 1> values{id};
    const auto leaf = write_adjacency_leaf(store, values);
    return leaf ? std::expected<AdjacencyDescriptor, Error>(
                      AdjacencyDescriptor{*leaf, 0, 1})
                : std::expected<AdjacencyDescriptor, Error>(
                      std::unexpected(leaf.error()));
  }

  // Insertions extend the rightmost leaf. Rewriting the leaf and its ancestors
  // gives copy-on-write sharing; a full level propagates one new child upward.
  std::vector<std::vector<PageId>> path;
  PageId current = descriptor.root;
  try {
    for (std::uint32_t level = descriptor.height; level > 0; --level) {
      const auto children = read_adjacency_reference(store, current);
      if (!children || children->empty())
        return std::unexpected(children ? Error::invalid_page
                                        : children.error());
      path.push_back(*children);
      current = children->back();
    }
    const auto leaf = read_adjacency_leaf(store, current);
    if (!leaf) return std::unexpected(leaf.error());

    PageId carry = invalid_page_id;
    bool append_child = leaf->size() == adjacency_page_capacity;
    if (!append_child) {
      std::vector<PersonId> replacement = *leaf;
      replacement.push_back(id);
      const auto page = write_adjacency_leaf(store, replacement);
      if (!page) return std::unexpected(page.error());
      carry = *page;
      for (std::size_t index = path.size(); index > 0; --index) {
        auto children = path[index - 1];
        children.back() = carry;
        const auto page = write_adjacency_reference(store, children);
        if (!page) return std::unexpected(page.error());
        carry = *page;
      }
      return AdjacencyDescriptor{carry, descriptor.height,
                                 descriptor.count + 1};
    }

    const std::array<PersonId, 1> value{id};
    const auto new_leaf = write_adjacency_leaf(store, value);
    if (!new_leaf) return std::unexpected(new_leaf.error());
    carry = *new_leaf;
    for (std::size_t index = path.size(); index > 0; --index) {
      const std::size_t node_index = index - 1;
      auto children = path[node_index];
      if (children.size() < adjacency_tree_fanout) {
        children.push_back(carry);
      } else {
        const std::array<PageId, 1> split{carry};
        const auto page = write_adjacency_reference(store, split);
        if (!page) return std::unexpected(page.error());
        carry = *page;
        continue;
      }
      const auto page = write_adjacency_reference(store, children);
      if (!page) return std::unexpected(page.error());
      carry = *page;
      for (std::size_t parent = node_index; parent > 0; --parent) {
        auto ancestors = path[parent - 1];
        ancestors.back() = carry;
        const auto ancestor_page = write_adjacency_reference(store, ancestors);
        if (!ancestor_page) return std::unexpected(ancestor_page.error());
        carry = *ancestor_page;
      }
      return AdjacencyDescriptor{carry, descriptor.height,
                                 descriptor.count + 1};
    }
    if (descriptor.height == UINT32_MAX)
      return std::unexpected(Error::page_limit);
    const std::array<PageId, 2> root{descriptor.root, carry};
    const auto page = write_adjacency_reference(store, root);
    return page ? std::expected<AdjacencyDescriptor, Error>(AdjacencyDescriptor{
                      *page, descriptor.height + 1, descriptor.count + 1})
                : std::expected<AdjacencyDescriptor, Error>(
                      std::unexpected(page.error()));
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::out_of_memory);
  }
}

auto adjacency_remove(PageStore& store, const AdjacencyDescriptor& descriptor,
                      PersonId id)
    -> std::expected<AdjacencyDescriptor, Error> {
  if (descriptor.count == 0) return descriptor;
  struct Step {
    std::vector<PageId> children;
    std::size_t index = 0;
  };
  std::vector<Step> path;
  std::vector<PersonId> target;
  std::size_t target_index = 0;
  // Unlike insertion, removal has no index by member value and searches leaves
  // depth-first. It rewrites only the path that contained the member, then
  // removes empty branches and collapses single-child roots.
  std::function<std::expected<bool, Error>(PageId, std::uint32_t)> find =
      [&](PageId page, std::uint32_t height) -> std::expected<bool, Error> {
    if (height == 0) {
      const auto leaf = read_adjacency_leaf(store, page);
      if (!leaf) return std::unexpected(leaf.error());
      const auto found = std::find(leaf->begin(), leaf->end(), id);
      if (found == leaf->end()) return false;
      target = *leaf;
      target_index = static_cast<std::size_t>(found - leaf->begin());
      return true;
    }
    const auto children = read_adjacency_reference(store, page);
    if (!children) return std::unexpected(children.error());
    for (std::size_t index = 0; index < children->size(); ++index) {
      path.push_back({*children, index});
      const auto found = find((*children)[index], height - 1);
      if (!found) return std::unexpected(found.error());
      if (*found) return true;
      path.pop_back();
    }
    return false;
  };
  const auto found = find(descriptor.root, descriptor.height);
  if (!found) return std::unexpected(found.error());
  if (!*found) return descriptor;
  if (descriptor.count == 1) return AdjacencyDescriptor{};
  target[target_index] = target.back();
  target.pop_back();
  PageId carry = invalid_page_id;
  if (!target.empty()) {
    const auto replacement = write_adjacency_leaf(store, target);
    if (!replacement) return std::unexpected(replacement.error());
    carry = *replacement;
  }
  for (std::size_t level = path.size(); level > 0; --level) {
    Step& step = path[level - 1];
    if (carry == invalid_page_id) {
      step.children.erase(step.children.begin() +
                          static_cast<std::ptrdiff_t>(step.index));
    } else {
      step.children[step.index] = carry;
    }
    if (step.children.empty()) {
      carry = invalid_page_id;
      continue;
    }
    const auto parent = write_adjacency_reference(store, step.children);
    if (!parent) return std::unexpected(parent.error());
    carry = *parent;
  }
  if (carry == invalid_page_id) return AdjacencyDescriptor{};
  std::uint32_t height = descriptor.height;
  while (height > 0) {
    const auto children = read_adjacency_reference(store, carry);
    if (!children) return std::unexpected(children.error());
    if (children->size() != 1) break;
    carry = children->front();
    --height;
  }
  return AdjacencyDescriptor{carry, height, descriptor.count - 1};
}

auto for_each_adjacency(
    const PageStore& store, const AdjacencyDescriptor& descriptor,
    const std::function<std::expected<void, Error>(PersonId)>& callback)
    -> std::expected<void, Error> {
  if (descriptor.count == 0)
    return descriptor.root == invalid_page_id && descriptor.height == 0
               ? std::expected<void, Error>{}
               : std::expected<void, Error>(
                     std::unexpected(Error::invalid_page));
  if (descriptor.root == invalid_page_id)
    return std::unexpected(Error::invalid_page);
  std::uint64_t seen = 0;
  const auto result =
      traverse(store, descriptor.root, descriptor.height, callback, seen);
  if (!result) return result;
  return seen == descriptor.count
             ? std::expected<void, Error>{}
             : std::expected<void, Error>(std::unexpected(Error::invalid_page));
}

auto traverse_adjacency_pages(
    const PageStore& store, const AdjacencyDescriptor& descriptor,
    PageId extent,
    const std::function<std::expected<void, Error>(PageId, PageType)>& callback)
    -> std::expected<void, Error> {
  if (descriptor.count == 0)
    return descriptor.root == invalid_page_id && descriptor.height == 0
               ? std::expected<void, Error>{}
               : std::expected<void, Error>(
                     std::unexpected(Error::invalid_page));
  if (descriptor.root == invalid_page_id || descriptor.root >= extent)
    return std::unexpected(Error::invalid_page);
  try {
    // Tree pages should not be shared within one descriptor and cannot cycle.
    // Track visited pages anyway: a malformed reference page must fail rather
    // than recurse indefinitely or let one physical page inflate `count`.
    std::unordered_set<PageId> visited;
    std::uint64_t count = 0;
    std::function<std::expected<void, Error>(PageId, std::uint32_t)> visit =
        [&](PageId page, std::uint32_t height) -> std::expected<void, Error> {
      if (page == invalid_page_id || page >= extent ||
          !visited.insert(page).second)
        return std::unexpected(Error::invalid_page);
      if (height == 0) {
        const auto leaf = read_adjacency_leaf(store, page);
        if (!leaf) return std::unexpected(leaf.error());
        const auto called = callback(page, PageType::adjacency_leaf);
        if (!called) return std::unexpected(called.error());
        if (count > UINT64_MAX - leaf->size())
          return std::unexpected(Error::page_limit);
        count += leaf->size();
        return {};
      }
      const auto children = read_adjacency_reference(store, page);
      if (!children || children->empty())
        return std::unexpected(children ? Error::invalid_page
                                        : children.error());
      const auto called = callback(page, PageType::adjacency_reference);
      if (!called) return std::unexpected(called.error());
      for (const PageId child : *children) {
        const auto result = visit(child, height - 1);
        if (!result) return result;
      }
      return {};
    };
    const auto result = visit(descriptor.root, descriptor.height);
    if (!result) return result;
    return count == descriptor.count
               ? std::expected<void, Error>{}
               : std::expected<void, Error>(
                     std::unexpected(Error::invalid_page));
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::out_of_memory);
  }
}

auto adjacency_contains(const PageStore& store,
                        const AdjacencyDescriptor& descriptor, PersonId id)
    -> std::expected<bool, Error> {
  bool found = false;
  const auto result = for_each_adjacency(
      store, descriptor,
      [&found, id](PersonId candidate) -> std::expected<void, Error> {
        found = found || candidate == id;
        return std::expected<void, Error>{};
      });
  return result ? std::expected<bool, Error>(found)
                : std::expected<bool, Error>(std::unexpected(result.error()));
}

}  // namespace tavern
