#pragma once

#include <memory>
#include <tavern/database.hpp>

#include "commit.hpp"
#include "page_liveness_index.hpp"
#include "persistent_relationship.hpp"

namespace tavern {

struct FreePagePool;

struct Database::Impl {
  PageStore store;
  CommitContext commit;
  bool allocation_session_active = false;
  std::unique_ptr<FreePagePool> retained_free_page_pool;
  std::unique_ptr<PageLivenessIndex> page_liveness_shadow;
  PageLivenessUpdateDiagnostics page_liveness_shadow_diagnostics;

  Impl();
  Impl(PageStore&& page_store, CommitContext context);
  ~Impl();
};

Database::Impl& database_impl(Database& database);
const Database::Impl& database_impl(const Database& database);
bool database_usable_for_operation(const Database& database);
Error database_unavailable_error(const Database& database);

std::expected<Database, Error> create_database(const std::filesystem::path&);
std::expected<Database, Error> open_database(const std::filesystem::path&);
std::expected<void, Error> close_database(Database&);
std::expected<void, Error> initialize_page_liveness_shadow_for_test(Database&);
std::expected<std::vector<PageId>, Error>
page_liveness_shadow_candidates_for_test(const Database&);
std::expected<void, Error> follow_with_index_reuse_for_test(Database&, PersonId,
                                                            PersonId);
std::expected<void, Error> follow(Database&, PersonId, PersonId);
std::expected<void, Error> unfollow(Database&, PersonId, PersonId);
std::expected<void, Error> befriend(Database&, PersonId, PersonId);
std::expected<void, Error> unfriend(Database&, PersonId, PersonId);
std::expected<bool, Error> is_following(const Database&, PersonId, PersonId);
std::expected<bool, Error> are_friends(const Database&, PersonId, PersonId);
std::expected<void, Error> following(
    const Database&, PersonId,
    const std::function<std::expected<void, Error>(PersonId)>&);
std::expected<void, Error> followers(
    const Database&, PersonId,
    const std::function<std::expected<void, Error>(PersonId)>&);
std::expected<void, Error> friends(
    const Database&, PersonId,
    const std::function<std::expected<void, Error>(PersonId)>&);
std::expected<std::vector<PersonId>, Error> mutual_friends(const Database&,
                                                           PersonId, PersonId);

}  // namespace tavern
