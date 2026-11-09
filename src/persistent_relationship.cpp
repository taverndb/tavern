#include "persistent_relationship.hpp"

#include <limits>
#include <new>
#include <stdexcept>
#include <unordered_set>

namespace tavern {
namespace {
auto patron(const PageStore& s, PageId r, PageId extent, PersonId id)
    -> std::expected<std::optional<PatronRecord>, Error> {
  return directory_lookup(s, r, extent, id);
}
auto enumerate(const PageStore& s, PageId r, PageId extent, PersonId id,
               const AdjacencyDescriptor PatronRecord::* member,
               const std::function<std::expected<void, Error>(PersonId)>& cb)
    -> std::expected<void, Error> {
  const auto p = patron(s, r, extent, id);
  if (!p) return std::unexpected(p.error());
  return *p ? for_each_adjacency(s, (**p).*member, cb)
            : std::expected<void, Error>{};
}
}  // namespace
auto persistent_is_following(const PageStore& s, PageId r, PageId extent,
                             PersonId a, PersonId b)
    -> std::expected<bool, Error> {
  const auto p = patron(s, r, extent, a);
  if (!p) return std::unexpected(p.error());
  return *p ? adjacency_contains(s, (**p).following, b)
            : std::expected<bool, Error>(false);
}
auto persistent_are_friends(const PageStore& s, PageId r, PageId extent,
                            PersonId a, PersonId b)
    -> std::expected<bool, Error> {
  const auto p = patron(s, r, extent, a);
  if (!p) return std::unexpected(p.error());
  return *p ? adjacency_contains(s, (**p).friends, b)
            : std::expected<bool, Error>(false);
}
auto persistent_follow(PageStore& s, PageId root, PageId extent, PersonId a,
                       PersonId b) -> std::expected<PageId, Error> {
  if (a == b) return std::unexpected(Error::self_relationship);
  const auto pa = patron(s, root, extent, a);
  const auto pb = patron(s, root, extent, b);
  if (!pa) return std::unexpected(pa.error());
  if (!pb) return std::unexpected(pb.error());
  PatronRecord first{};
  first.id = a;
  PatronRecord second{};
  second.id = b;
  if (*pa) first = **pa;
  if (*pb) second = **pb;
  const auto forward = adjacency_contains(s, first.following, b);
  const auto reverse = adjacency_contains(s, second.followers, a);
  if (!forward) return std::unexpected(forward.error());
  if (!reverse) return std::unexpected(reverse.error());
  if (*forward && *reverse) return root;
  if (*forward != *reverse) return std::unexpected(Error::invariant_violation);
  const auto following = adjacency_insert(s, first.following, b);
  if (!following) return std::unexpected(following.error());
  const auto followers = adjacency_insert(s, second.followers, a);
  if (!followers) return std::unexpected(followers.error());
  first.following = *following;
  second.followers = *followers;
  const auto first_root =
      *pa ? directory_update(s, root, first) : directory_insert(s, root, first);
  if (!first_root) return std::unexpected(first_root.error());
  const auto final_root = *pb ? directory_update(s, *first_root, second)
                              : directory_insert(s, *first_root, second);
  return final_root;
}
auto persistent_unfollow(PageStore& s, PageId root, PageId extent, PersonId a,
                         PersonId b) -> std::expected<PageId, Error> {
  if (a == b) return std::unexpected(Error::self_relationship);
  const auto pa = patron(s, root, extent, a);
  const auto pb = patron(s, root, extent, b);
  if (!pa) return std::unexpected(pa.error());
  if (!pb) return std::unexpected(pb.error());
  if (!*pa && !*pb) return root;
  PatronRecord first{};
  first.id = a;
  PatronRecord second{};
  second.id = b;
  if (*pa) first = **pa;
  if (*pb) second = **pb;
  const auto forward = adjacency_contains(s, first.following, b);
  const auto reverse = adjacency_contains(s, second.followers, a);
  if (!forward) return std::unexpected(forward.error());
  if (!reverse) return std::unexpected(reverse.error());
  if (!*forward && !*reverse) return root;
  if (*forward != *reverse) return std::unexpected(Error::invariant_violation);
  const auto following = adjacency_remove(s, first.following, b);
  const auto followers = adjacency_remove(s, second.followers, a);
  if (!following) return std::unexpected(following.error());
  if (!followers) return std::unexpected(followers.error());
  first.following = *following;
  second.followers = *followers;
  const auto empty = [](const PatronRecord& record) -> bool {
    return record.following.count == 0 && record.followers.count == 0 &&
           record.friends.count == 0;
  };
  const auto first_root = empty(first) ? directory_remove(s, root, a)
                                       : directory_update(s, root, first);
  if (!first_root) return std::unexpected(first_root.error());
  return empty(second) ? directory_remove(s, *first_root, b)
                       : directory_update(s, *first_root, second);
}
auto persistent_befriend(PageStore& s, PageId root, PageId extent, PersonId a,
                         PersonId b) -> std::expected<PageId, Error> {
  if (a == b) return std::unexpected(Error::self_relationship);
  const auto pa = patron(s, root, extent, a);
  const auto pb = patron(s, root, extent, b);
  if (!pa) return std::unexpected(pa.error());
  if (!pb) return std::unexpected(pb.error());
  PatronRecord first{};
  first.id = a;
  PatronRecord second{};
  second.id = b;
  if (*pa) first = **pa;
  if (*pb) second = **pb;
  const auto left = adjacency_contains(s, first.friends, b);
  const auto right = adjacency_contains(s, second.friends, a);
  if (!left) return std::unexpected(left.error());
  if (!right) return std::unexpected(right.error());
  if (*left && *right) return root;
  if (*left != *right) return std::unexpected(Error::invariant_violation);
  const auto first_friends = adjacency_insert(s, first.friends, b);
  const auto second_friends = adjacency_insert(s, second.friends, a);
  if (!first_friends) return std::unexpected(first_friends.error());
  if (!second_friends) return std::unexpected(second_friends.error());
  first.friends = *first_friends;
  second.friends = *second_friends;
  const auto first_root =
      *pa ? directory_update(s, root, first) : directory_insert(s, root, first);
  if (!first_root) return std::unexpected(first_root.error());
  return *pb ? directory_update(s, *first_root, second)
             : directory_insert(s, *first_root, second);
}
auto persistent_unfriend(PageStore& s, PageId root, PageId extent, PersonId a,
                         PersonId b) -> std::expected<PageId, Error> {
  if (a == b) return std::unexpected(Error::self_relationship);
  const auto pa = patron(s, root, extent, a);
  const auto pb = patron(s, root, extent, b);
  if (!pa) return std::unexpected(pa.error());
  if (!pb) return std::unexpected(pb.error());
  if (!*pa && !*pb) return root;
  PatronRecord first{};
  first.id = a;
  PatronRecord second{};
  second.id = b;
  if (*pa) first = **pa;
  if (*pb) second = **pb;
  const auto left = adjacency_contains(s, first.friends, b);
  const auto right = adjacency_contains(s, second.friends, a);
  if (!left) return std::unexpected(left.error());
  if (!right) return std::unexpected(right.error());
  if (!*left && !*right) return root;
  if (*left != *right) return std::unexpected(Error::invariant_violation);
  const auto first_friends = adjacency_remove(s, first.friends, b);
  const auto second_friends = adjacency_remove(s, second.friends, a);
  if (!first_friends) return std::unexpected(first_friends.error());
  if (!second_friends) return std::unexpected(second_friends.error());
  first.friends = *first_friends;
  second.friends = *second_friends;
  const auto empty = [](const PatronRecord& r) -> bool {
    return r.following.count == 0 && r.followers.count == 0 &&
           r.friends.count == 0;
  };
  const auto first_root = empty(first) ? directory_remove(s, root, a)
                                       : directory_update(s, root, first);
  if (!first_root) return std::unexpected(first_root.error());
  return empty(second) ? directory_remove(s, *first_root, b)
                       : directory_update(s, *first_root, second);
}
auto for_each_persistent_following(
    const PageStore& s, PageId r, PageId extent, PersonId id,
    const std::function<std::expected<void, Error>(PersonId)>& cb)
    -> std::expected<void, Error> {
  return enumerate(s, r, extent, id, &PatronRecord::following, cb);
}
auto for_each_persistent_followers(
    const PageStore& s, PageId r, PageId extent, PersonId id,
    const std::function<std::expected<void, Error>(PersonId)>& cb)
    -> std::expected<void, Error> {
  return enumerate(s, r, extent, id, &PatronRecord::followers, cb);
}
auto for_each_persistent_friends(
    const PageStore& s, PageId r, PageId extent, PersonId id,
    const std::function<std::expected<void, Error>(PersonId)>& cb)
    -> std::expected<void, Error> {
  return enumerate(s, r, extent, id, &PatronRecord::friends, cb);
}
auto persistent_mutual_friends(const PageStore& s, PageId r, PageId extent,
                               PersonId a, PersonId b)
    -> std::expected<std::vector<PersonId>, Error> {
  const auto pa = patron(s, r, extent, a);
  const auto pb = patron(s, r, extent, b);
  if (!pa) return std::unexpected(pa.error());
  if (!pb) return std::unexpected(pb.error());
  if (!*pa || !*pb) return std::vector<PersonId>{};
  const AdjacencyDescriptor& first = (**pa).friends;
  const AdjacencyDescriptor& second = (**pb).friends;
  const AdjacencyDescriptor& smaller =
      first.count <= second.count ? first : second;
  const AdjacencyDescriptor& larger =
      first.count <= second.count ? second : first;
  if (smaller.count > std::numeric_limits<std::size_t>::max())
    return std::unexpected(Error::page_limit);
  try {
    std::unordered_set<PersonId> members;
    if (smaller.count > members.max_size())
      return std::unexpected(Error::page_limit);
    members.reserve(static_cast<std::size_t>(smaller.count));
    const auto collected = for_each_adjacency(
        s, smaller, [&](PersonId id) -> std::expected<void, Error> {
          members.insert(id);
          return std::expected<void, Error>{};
        });
    if (!collected) return std::unexpected(collected.error());
    std::vector<PersonId> out;
    const auto intersected = for_each_adjacency(
        s, larger, [&](PersonId id) -> std::expected<void, Error> {
          if (members.erase(id) != 0) out.push_back(id);
          return std::expected<void, Error>{};
        });
    return intersected ? std::expected<std::vector<PersonId>, Error>(out)
                       : std::expected<std::vector<PersonId>, Error>(
                             std::unexpected(intersected.error()));
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::out_of_memory);
  } catch (const std::length_error&) {
    return std::unexpected(Error::page_limit);
  }
}
}  // namespace tavern
