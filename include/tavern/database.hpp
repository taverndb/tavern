#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace tavern {

// Person identifiers are application-defined unsigned values. They are not
// allocated, normalized, or otherwise interpreted by Tavern.
using PersonId = std::uint64_t;

// Operations return failures rather than throw. I/O and corruption errors can
// arise from a later page read as well as from a mutation. `recovery_required`
// means an earlier commit's metadata write had an indeterminate outcome; close
// and reopen the database before attempting another operation.
enum class Error {
  self_relationship,
  out_of_memory,
  block_capacity_exhausted,
  io_failure,
  invalid_page,
  invalid_metadata,
  page_limit,
  directory_exists,
  directory_missing,
  directory_capacity_exhausted,
  invalid_directory,
  invariant_violation,
  recovery_required,
  commit_not_published,
  database_locked,
};

// Move-only owner of one exclusively locked database file. `create` requires a
// new path; `open` recovers the newest valid committed generation. Destruction
// releases the file descriptor and lock, but cannot report a close failure, so
// applications that need that diagnostic should call close explicitly.
//
// Successful mutations are durably published as complete generations. Repeating
// a relationship addition or removing an absent relationship succeeds without
// changing the database. `follow(a, b)` is directed; `befriend(a, b)` maintains
// a symmetric relationship. Self-relationships fail.
//
// The const query operations do not publish changes. Enumeration callbacks
// stream identifiers rather than materializing them, and propagate callback or
// storage errors; vector-returning counterparts materialize the same
// unspecified order. Database instances do not provide concurrent-operation
// synchronization.
class Database {
 public:
  // Creation initializes an empty database. Opening acquires the same exclusive
  // advisory lock used by writers, so another cooperating Tavern process cannot
  // open the file concurrently.
  static auto create(const std::filesystem::path& path)
      -> std::expected<Database, Error>;
  static auto open(const std::filesystem::path& path)
      -> std::expected<Database, Error>;

  ~Database();
  Database(Database&&) noexcept;
  auto operator=(Database&&) noexcept -> Database&;
  Database(const Database&) = delete;
  auto operator=(const Database&) -> Database& = delete;

  // Releases the descriptor and advisory lock. An explicitly closed instance is
  // no longer usable; close reports an OS close error when one is observable.
  auto close() -> std::expected<void, Error>;
  auto is_open() const noexcept -> bool;

  // Each successful mutation is atomic with respect to process crash recovery:
  // reopening observes either the preceding committed generation or this one.
  auto follow(PersonId from, PersonId to) -> std::expected<void, Error>;
  auto unfollow(PersonId from, PersonId to) -> std::expected<void, Error>;
  auto befriend(PersonId a, PersonId b) -> std::expected<void, Error>;
  auto unfriend(PersonId a, PersonId b) -> std::expected<void, Error>;

  // Read-only membership queries; false includes a person with no record.
  auto is_following(PersonId from, PersonId to) const
      -> std::expected<bool, Error>;
  auto are_friends(PersonId a, PersonId b) const -> std::expected<bool, Error>;

  auto for_each_following(
      PersonId person,
      const std::function<std::expected<void, Error>(PersonId)>& callback) const
      -> std::expected<void, Error>;
  auto for_each_follower(
      PersonId person,
      const std::function<std::expected<void, Error>(PersonId)>& callback) const
      -> std::expected<void, Error>;
  auto for_each_friend(
      PersonId person,
      const std::function<std::expected<void, Error>(PersonId)>& callback) const
      -> std::expected<void, Error>;

  // Materialized convenience forms of the streaming enumerations. Result order
  // is intentionally unspecified and must not be used as an ordering contract.
  auto following(PersonId person) const
      -> std::expected<std::vector<PersonId>, Error>;
  auto followers(PersonId person) const
      -> std::expected<std::vector<PersonId>, Error>;
  auto friends(PersonId person) const
      -> std::expected<std::vector<PersonId>, Error>;
  auto mutual_friends(PersonId a, PersonId b) const
      -> std::expected<std::vector<PersonId>, Error>;

 private:
  struct Impl;
  explicit Database(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;

  friend auto database_impl(Database&) -> Impl&;
  friend auto database_impl(const Database&) -> const Impl&;
};

}  // namespace tavern
