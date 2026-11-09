#include "database.hpp"

#include "free_page_pool.hpp"
#include "page_allocation_session.hpp"
namespace tavern {
Database::Impl::Impl() = default;
Database::Impl::Impl(PageStore&& page_store, CommitContext context)
    : store(std::move(page_store)), commit(context) {}
Database::Impl::~Impl() = default;
Database::Database(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
// Impl owns PageStore, whose destruction closes the descriptor. This RAII path
// cannot surface close errors and does not add a synchronization step; explicit
// close is available when callers need an error result.
Database::~Database() = default;
Database::Database(Database&& other) noexcept : impl_(std::move(other.impl_)) {
  if (impl_) {
    impl_->retained_free_page_pool.reset();
    impl_->page_liveness_shadow.reset();
  }
}
auto Database::operator=(Database&& other) noexcept -> Database& {
  if (this != &other) {
    impl_ = std::move(other.impl_);
    if (impl_) {
      impl_->retained_free_page_pool.reset();
      impl_->page_liveness_shadow.reset();
    }
  }
  return *this;
}
auto database_impl(Database& database) -> Database::Impl& {
  return *database.impl_;
}
auto database_impl(const Database& database) -> const Database::Impl& {
  return *database.impl_;
}
auto Database::is_open() const noexcept -> bool {
  return impl_ && impl_->store.file_descriptor >= 0;
}
auto Database::close() -> std::expected<void, Error> {
  return close_database(*this);
}

auto Database::follow(PersonId from, PersonId to)
    -> std::expected<void, Error> {
  return tavern::follow(*this, from, to);
}
auto Database::unfollow(PersonId from, PersonId to)
    -> std::expected<void, Error> {
  return tavern::unfollow(*this, from, to);
}
auto Database::befriend(PersonId a, PersonId b) -> std::expected<void, Error> {
  return tavern::befriend(*this, a, b);
}
auto Database::unfriend(PersonId a, PersonId b) -> std::expected<void, Error> {
  return tavern::unfriend(*this, a, b);
}
auto Database::is_following(PersonId from, PersonId to) const
    -> std::expected<bool, Error> {
  return tavern::is_following(*this, from, to);
}
auto Database::are_friends(PersonId a, PersonId b) const
    -> std::expected<bool, Error> {
  return tavern::are_friends(*this, a, b);
}
auto Database::for_each_following(
    PersonId person,
    const std::function<std::expected<void, Error>(PersonId)>& callback) const
    -> std::expected<void, Error> {
  return tavern::following(*this, person, callback);
}
auto Database::for_each_follower(
    PersonId person,
    const std::function<std::expected<void, Error>(PersonId)>& callback) const
    -> std::expected<void, Error> {
  return tavern::followers(*this, person, callback);
}
auto Database::for_each_friend(
    PersonId person,
    const std::function<std::expected<void, Error>(PersonId)>& callback) const
    -> std::expected<void, Error> {
  return tavern::friends(*this, person, callback);
}
namespace {
template <typename Enumeration>
auto collect(Enumeration enumeration)
    -> std::expected<std::vector<PersonId>, Error> {
  // Vector APIs are deliberately wrappers around the streaming traversal, so
  // they preserve its order and error behavior while paying O(result size)
  // memory only for callers that request materialization.
  try {
    std::vector<PersonId> result;
    const auto enumerated = enumeration([&](PersonId person) -> auto {
      try {
        result.push_back(person);
        return std::expected<void, Error>{};
      } catch (const std::bad_alloc&) {
        return std::expected<void, Error>(
            std::unexpected(Error::out_of_memory));
      }
    });
    if (!enumerated) return std::unexpected(enumerated.error());
    return result;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::out_of_memory);
  }
}
}  // namespace
auto Database::following(PersonId person) const
    -> std::expected<std::vector<PersonId>, Error> {
  return collect([&](const auto& callback) -> auto {
    return for_each_following(person, callback);
  });
}
auto Database::followers(PersonId person) const
    -> std::expected<std::vector<PersonId>, Error> {
  return collect([&](const auto& callback) -> auto {
    return for_each_follower(person, callback);
  });
}
auto Database::friends(PersonId person) const
    -> std::expected<std::vector<PersonId>, Error> {
  return collect([&](const auto& callback) -> auto {
    return for_each_friend(person, callback);
  });
}
auto Database::mutual_friends(PersonId a, PersonId b) const
    -> std::expected<std::vector<PersonId>, Error> {
  return tavern::mutual_friends(*this, a, b);
}

auto Database::create(const std::filesystem::path& path)
    -> std::expected<Database, Error> {
  auto store = create_page_store(path);
  if (!store) return std::unexpected(store.error());
  auto commit = initialize_commit_context(*store);
  if (!commit) return std::unexpected(commit.error());
  if (commit->root != invalid_page_id || commit->generation != 0 ||
      commit->extent != first_data_page)
    return std::unexpected(Error::invalid_metadata);
  return Database{std::make_unique<Database::Impl>(std::move(*store), *commit)};
}
auto Database::open(const std::filesystem::path& path)
    -> std::expected<Database, Error> {
  auto store = open_page_store(path);
  if (!store) return std::unexpected(store.error());
  // Recovery validates both independently written slots and selects the larger
  // generation among those that decode. Equal generations prefer slot A. This
  // is not a repair pass: malformed pages reachable from the selected root are
  // detected only as later operations traverse them.
  const auto first = read_metadata(*store, metadata_slot_a);
  const auto second = read_metadata(*store, metadata_slot_b);
  if (!first && !second) return std::unexpected(Error::invalid_metadata);
  const bool choose_second =
      second && (!first || second->generation > first->generation);
  const auto& metadata = choose_second ? second : first;
  if (metadata->committed_page_count > store->page_count)
    return std::unexpected(Error::invalid_metadata);
  if (metadata->root != invalid_page_id) {
    const auto root = read_page(*store, metadata->root);
    if (!root || (root->type != PageType::directory_leaf &&
                  root->type != PageType::directory_node))
      return std::unexpected(Error::invalid_metadata);
  }
  return Database{std::make_unique<Database::Impl>(
      std::move(*store),
      CommitContext{metadata->root, metadata->generation,
                    metadata->committed_page_count,
                    choose_second ? metadata_slot_b : metadata_slot_a, false})};
}
auto create_database(const std::filesystem::path& path)
    -> std::expected<Database, Error> {
  return Database::create(path);
}
auto open_database(const std::filesystem::path& path)
    -> std::expected<Database, Error> {
  return Database::open(path);
}
auto close_database(Database& database) -> std::expected<void, Error> {
  if (!database.is_open()) return std::unexpected(Error::io_failure);
  auto& state = database_impl(database);
  if (state.allocation_session_active)
    return std::unexpected(Error::invalid_page);
  state.retained_free_page_pool.reset();
  state.page_liveness_shadow.reset();
  return close_page_store(state.store);
}
auto database_usable_for_operation(const Database& d) -> bool {
  return d.is_open() && !database_impl(d).commit.recovery_required &&
         !database_impl(d).allocation_session_active;
}
auto database_unavailable_error(const Database& d) -> Error {
  return d.is_open() && database_impl(d).commit.recovery_required
             ? Error::recovery_required
             : Error::io_failure;
}
namespace {
template <typename Mutation>
auto mutate(Database& d, PersonId a, PersonId b, Mutation mutation)
    -> std::expected<void, Error> {
  if (!database_usable_for_operation(d))
    return std::unexpected(database_unavailable_error(d));
  const auto root =
      mutation(database_impl(d).store, database_impl(d).commit.root,
               database_impl(d).commit.extent, a, b);
  if (!root) return std::unexpected(root.error());
  // A structurally unchanged operation is an idempotent no-op and needs no
  // publication. Otherwise commit_root supplies durable, generation-atomic
  // visibility or reports that recovery is required after an uncertain write.
  return *root == database_impl(d).commit.root
             ? std::expected<void, Error>{}
             : commit_root(database_impl(d).store, database_impl(d).commit,
                           *root);
}
template <typename Mutation, typename Query>
auto mutate_with_reuse(Database& d, PersonId a, PersonId b,
                       bool expected_present, Mutation mutation, Query query)
    -> std::expected<void, Error> {
  if (!database_usable_for_operation(d))
    return std::unexpected(database_unavailable_error(d));
  if (a == b) return std::unexpected(Error::self_relationship);
  const auto present =
      query(database_impl(d).store, database_impl(d).commit.root,
            database_impl(d).commit.extent, a, b);
  if (!present) return std::unexpected(present.error());
  if (*present == expected_present) return {};
  // A retained pool is a snapshot of one exact committed state. Any mismatch
  // (including physical growth) discards it and triggers a fresh scan rather
  // than risking reuse based on stale reachability.
  if (database_impl(d).retained_free_page_pool &&
      !free_page_pool_matches(*database_impl(d).retained_free_page_pool, d))
    database_impl(d).retained_free_page_pool.reset();
  FreePagePool pool;
  if (database_impl(d).retained_free_page_pool) {
    pool = std::move(*database_impl(d).retained_free_page_pool);
    database_impl(d).retained_free_page_pool.reset();
  } else {
    auto fresh = build_free_page_pool(d);
    if (!fresh) return std::unexpected(fresh.error());
    pool = std::move(*fresh);
  }
  PageId root = invalid_page_id;
  FreePagePool remaining;
  {
    // The session owns the pool while the mutation constructs replacement
    // pages. Its destructor releases the allocator hook before metadata is
    // published; allocated pages remain unreachable until commit_root succeeds.
    auto session =
        storage_detail::begin_page_allocation_session(d, std::move(pool));
    if (!session) return std::unexpected(session.error());
    const auto candidate =
        mutation(database_impl(d).store, database_impl(d).commit.root,
                 database_impl(d).commit.extent, a, b);
    if (!candidate) return std::unexpected(candidate.error());
    root = *candidate;
    remaining = std::move(session->pool);
  }
  if (root == database_impl(d).commit.root) {
    if (!remaining.candidates.empty())
      database_impl(d).retained_free_page_pool =
          std::make_unique<FreePagePool>(std::move(remaining));
    return {};
  }
  const auto committed =
      commit_root(database_impl(d).store, database_impl(d).commit, root);
  if (!committed) return std::unexpected(committed.error());
  if (!remaining.candidates.empty()) {
    advance_retained_free_page_pool(remaining, d);
    database_impl(d).retained_free_page_pool =
        std::make_unique<FreePagePool>(std::move(remaining));
  }
  return {};
}

}  // namespace
auto follow(Database& d, PersonId a, PersonId b) -> std::expected<void, Error> {
  return mutate_with_reuse(d, a, b, true, persistent_follow,
                           persistent_is_following);
}
auto unfollow(Database& d, PersonId a, PersonId b)
    -> std::expected<void, Error> {
  return mutate_with_reuse(d, a, b, false, persistent_unfollow,
                           persistent_is_following);
}
auto befriend(Database& d, PersonId a, PersonId b)
    -> std::expected<void, Error> {
  return mutate_with_reuse(d, a, b, true, persistent_befriend,
                           persistent_are_friends);
}
auto unfriend(Database& d, PersonId a, PersonId b)
    -> std::expected<void, Error> {
  return mutate_with_reuse(d, a, b, false, persistent_unfriend,
                           persistent_are_friends);
}
auto is_following(const Database& d, PersonId a, PersonId b)
    -> std::expected<bool, Error> {
  return database_usable_for_operation(d)
             ? persistent_is_following(database_impl(d).store,
                                       database_impl(d).commit.root,
                                       database_impl(d).commit.extent, a, b)
             : std::expected<bool, Error>(
                   std::unexpected(database_unavailable_error(d)));
}
auto are_friends(const Database& d, PersonId a, PersonId b)
    -> std::expected<bool, Error> {
  return database_usable_for_operation(d)
             ? persistent_are_friends(database_impl(d).store,
                                      database_impl(d).commit.root,
                                      database_impl(d).commit.extent, a, b)
             : std::expected<bool, Error>(
                   std::unexpected(database_unavailable_error(d)));
}
auto following(const Database& d, PersonId id,
               const std::function<std::expected<void, Error>(PersonId)>& cb)
    -> std::expected<void, Error> {
  return database_usable_for_operation(d)
             ? for_each_persistent_following(
                   database_impl(d).store, database_impl(d).commit.root,
                   database_impl(d).commit.extent, id, cb)
             : std::expected<void, Error>(
                   std::unexpected(database_unavailable_error(d)));
}
auto followers(const Database& d, PersonId id,
               const std::function<std::expected<void, Error>(PersonId)>& cb)
    -> std::expected<void, Error> {
  return database_usable_for_operation(d)
             ? for_each_persistent_followers(
                   database_impl(d).store, database_impl(d).commit.root,
                   database_impl(d).commit.extent, id, cb)
             : std::expected<void, Error>(
                   std::unexpected(database_unavailable_error(d)));
}
auto friends(const Database& d, PersonId id,
             const std::function<std::expected<void, Error>(PersonId)>& cb)
    -> std::expected<void, Error> {
  return database_usable_for_operation(d)
             ? for_each_persistent_friends(
                   database_impl(d).store, database_impl(d).commit.root,
                   database_impl(d).commit.extent, id, cb)
             : std::expected<void, Error>(
                   std::unexpected(database_unavailable_error(d)));
}
auto mutual_friends(const Database& d, PersonId a, PersonId b)
    -> std::expected<std::vector<PersonId>, Error> {
  return database_usable_for_operation(d)
             ? persistent_mutual_friends(database_impl(d).store,
                                         database_impl(d).commit.root,
                                         database_impl(d).commit.extent, a, b)
             : std::expected<std::vector<PersonId>, Error>(
                   std::unexpected(database_unavailable_error(d)));
}
}  // namespace tavern
