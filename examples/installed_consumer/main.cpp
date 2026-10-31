#include <algorithm>
#include <filesystem>
#include <tavern/database.hpp>

auto main(int argc, char** argv) -> int {
  if (argc != 2) return 1;
  const std::filesystem::path path = argv[1];
  std::filesystem::remove(path);
  auto database = tavern::Database::create(path);
  if (!database || !database->follow(1, 2) || !database->befriend(1, 3) ||
      !database->befriend(2, 3) ||
      !database->is_following(1, 2).value_or(false) ||
      !database->are_friends(3, 1).value_or(false)) {
    return 1;
  }
  std::size_t streamed = 0;
  const auto mutual = database->mutual_friends(1, 2);
  if (!database->for_each_friend(
          1,
          [&](tavern::PersonId) -> std::expected<void, tavern::Error> {
            ++streamed;
            return std::expected<void, tavern::Error>{};
          }) ||
      streamed != 1 || !mutual ||
      std::find(mutual->begin(), mutual->end(), 3) == mutual->end() ||
      !database->close()) {
    return 1;
  }
  auto reopened = tavern::Database::open(path);
  const bool durable =
      reopened && reopened->is_following(1, 2).value_or(false) &&
      reopened->are_friends(1, 3).value_or(false) && reopened->close();
  std::filesystem::remove(path);
  return durable ? 0 : 1;
}
