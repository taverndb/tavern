#pragma once
#include <functional>

#include "adjacency_tree.hpp"
namespace tavern {
std::expected<bool, Error> persistent_is_following(const PageStore&, PageId,
                                                   PageId, PersonId, PersonId);
std::expected<bool, Error> persistent_are_friends(const PageStore&, PageId,
                                                  PageId, PersonId, PersonId);
std::expected<PageId, Error> persistent_follow(PageStore&, PageId, PageId,
                                               PersonId, PersonId);
std::expected<PageId, Error> persistent_unfollow(PageStore&, PageId, PageId,
                                                 PersonId, PersonId);
std::expected<PageId, Error> persistent_befriend(PageStore&, PageId, PageId,
                                                 PersonId, PersonId);
std::expected<PageId, Error> persistent_unfriend(PageStore&, PageId, PageId,
                                                 PersonId, PersonId);
std::expected<void, Error> for_each_persistent_following(
    const PageStore&, PageId, PageId, PersonId,
    const std::function<std::expected<void, Error>(PersonId)>&);
std::expected<void, Error> for_each_persistent_followers(
    const PageStore&, PageId, PageId, PersonId,
    const std::function<std::expected<void, Error>(PersonId)>&);
std::expected<void, Error> for_each_persistent_friends(
    const PageStore&, PageId, PageId, PersonId,
    const std::function<std::expected<void, Error>(PersonId)>&);
std::expected<std::vector<PersonId>, Error> persistent_mutual_friends(
    const PageStore&, PageId, PageId, PersonId, PersonId);
}  // namespace tavern
