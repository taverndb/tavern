#include <algorithm>
#include <filesystem>
#include <tavern/database.hpp>

namespace {

auto contains(const std::vector<tavern::PersonId>& values,
              tavern::PersonId person) -> bool {
  return std::find(values.begin(), values.end(), person) != values.end();
}

}  // namespace

auto main() -> int {
  const auto directory = std::filesystem::temp_directory_path();
  const auto path = directory / "tavern-public-database-consumer.db";
  const auto second_path = directory / "tavern-public-database-consumer-2.db";
  const auto destructor_path =
      directory / "tavern-public-database-destructor.db";
  std::filesystem::remove(path);
  std::filesystem::remove(second_path);
  std::filesystem::remove(destructor_path);

  auto database = tavern::Database::create(path);
  if (!database || !database->is_open() || tavern::Database::create(path) ||
      tavern::Database::open(path) ||
      database->follow(1, 1).error() != tavern::Error::self_relationship ||
      !database->follow(1, 2) || !database->follow(1, 2) ||
      !database->befriend(1, 2) || !database->befriend(1, 3) ||
      !database->befriend(2, 3) || !database->follow(4, 1)) {
    return 1;
  }
  const auto follows = database->following(1);
  const auto followers = database->followers(1);
  const auto friends = database->friends(1);
  const auto mutual = database->mutual_friends(1, 2);
  if (!follows || !followers || !friends || !mutual || !contains(*follows, 2) ||
      contains(*follows, 3) || !contains(*followers, 4) ||
      !contains(*friends, 2) || !contains(*friends, 3) ||
      !contains(*mutual, 3) || !database->is_following(1, 2).value_or(false) ||
      database->is_following(2, 1).value_or(true) ||
      !database->are_friends(2, 1).value_or(false)) {
    return 1;
  }
  std::size_t streamed = 0;
  if (!database->for_each_friend(
          1,
          [&](tavern::PersonId) -> std::expected<void, tavern::Error> {
            ++streamed;
            return std::expected<void, tavern::Error>{};
          }) ||
      streamed != friends->size() ||
      database->for_each_following(
                  1,
                  [](tavern::PersonId) -> std::expected<void, tavern::Error> {
                    return std::expected<void, tavern::Error>(
                        std::unexpected(tavern::Error::invariant_violation));
                  })
              .error() != tavern::Error::invariant_violation ||
      !database->unfollow(1, 2) || !database->unfollow(1, 2) ||
      !database->unfriend(1, 2) || !database->unfriend(1, 2)) {
    return 1;
  }

  tavern::Database moved(std::move(*database));
  if (database->is_open() || database->is_following(1, 2) || !moved.is_open() ||
      !moved.close() || moved.is_open() || moved.close() ||
      moved.are_friends(1, 3)) {
    return 1;
  }
  auto reopened = tavern::Database::open(path);
  auto second = tavern::Database::create(second_path);
  if (!reopened || !second || !reopened->are_friends(1, 3).value_or(false))
    return 1;
  *second = std::move(*reopened);
  if (reopened->is_open() || !second->is_open() || !second->close()) return 1;
  {
    auto temporary = tavern::Database::create(destructor_path);
    if (!temporary || !temporary->is_open()) return 1;
  }
  auto after_destructor = tavern::Database::open(destructor_path);
  const bool result = after_destructor && after_destructor->is_open() &&
                      after_destructor->close();
  std::filesystem::remove(path);
  std::filesystem::remove(second_path);
  std::filesystem::remove(destructor_path);
  return result ? 0 : 1;
}
