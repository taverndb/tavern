#include <unistd.h>

#include <filesystem>

#include "database_internal.hpp"

namespace {

auto path() -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         ("tavern-production-reclamation-" +
          std::to_string(static_cast<long long>(getpid())) + ".db");
}

template <typename Mutation>
auto stale_after_normal_mutation(tavern::Database& database, Mutation mutation)
    -> bool {
  if (!tavern::initialize_page_liveness_shadow_for_test(database) ||
      !database_impl(database).page_liveness_shadow)
    return false;
  const auto roots =
      database_impl(database).page_liveness_shadow->protected_roots;
  if (!mutation() || !database_impl(database).page_liveness_shadow ||
      database_impl(database).page_liveness_shadow->protected_roots.size() !=
          roots.size() ||
      tavern::page_liveness_shadow_candidates_for_test(database)) {
    return false;
  }
  for (std::size_t index = 0; index < roots.size(); ++index) {
    const auto& before = roots[index];
    const auto& after =
        database_impl(database).page_liveness_shadow->protected_roots[index];
    if (before.metadata_slot != after.metadata_slot ||
        before.root != after.root || before.extent != after.extent ||
        before.generation != after.generation)
      return false;
  }
  return true;
}

}  // namespace

auto main() -> int {
  const auto file = path();
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  if (!database ||
      !stale_after_normal_mutation(*database,
                                   [&] -> std::expected<void, tavern::Error> {
                                     return tavern::follow(*database, 1, 2);
                                   }) ||
      !stale_after_normal_mutation(*database,
                                   [&] -> std::expected<void, tavern::Error> {
                                     return tavern::unfollow(*database, 1, 2);
                                   }) ||
      !stale_after_normal_mutation(*database,
                                   [&] -> std::expected<void, tavern::Error> {
                                     return tavern::befriend(*database, 3, 4);
                                   }) ||
      !stale_after_normal_mutation(*database,
                                   [&] -> std::expected<void, tavern::Error> {
                                     return tavern::unfriend(*database, 3, 4);
                                   }) ||
      !tavern::close_database(*database)) {
    std::filesystem::remove(file);
    return 1;
  }
  std::filesystem::remove(file);
  return 0;
}
