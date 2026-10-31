#include <unistd.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <unordered_set>
#include <vector>

#include "database_internal.hpp"
#include "free_page_pool.hpp"
#include "reachability.hpp"

namespace {

constexpr std::size_t population = 24;
constexpr std::size_t operations = 10000;
constexpr std::size_t checkpoint_interval = 100;
constexpr std::uint64_t seed = 0xC0FFEE1234567890ULL;

struct Model {
  std::array<std::array<bool, population>, population> follows{};
  std::array<std::array<bool, population>, population> friends{};
};

auto next(std::uint64_t& state) -> std::uint64_t {
  state ^= state << 7U;
  state ^= state >> 9U;
  state ^= state << 8U;
  return state;
}

auto path() -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         (std::string("tavern-durability-stress-") +
          std::to_string(static_cast<long long>(getpid())) + ".db");
}

auto matches(const tavern::Database& database, const Model& model) -> bool {
  for (std::size_t left = 0; left < population; ++left)
    for (std::size_t right = 0; right < population; ++right) {
      const auto following = tavern::is_following(
          database, static_cast<tavern::PersonId>(left + 1),
          static_cast<tavern::PersonId>(right + 1));
      const auto friendship =
          tavern::are_friends(database, static_cast<tavern::PersonId>(left + 1),
                              static_cast<tavern::PersonId>(right + 1));
      if (!following || !friendship ||
          *following != model.follows[left][right] ||
          *friendship != model.friends[left][right])
        return false;
    }
  return true;
}

auto matches_affected(const tavern::Database& database, const Model& model,
                      std::size_t left, std::size_t right) -> bool {
  for (const auto pair : {std::array<std::size_t, 2>{left, right},
                          std::array<std::size_t, 2>{right, left}}) {
    const auto following = tavern::is_following(
        database, static_cast<tavern::PersonId>(pair[0] + 1),
        static_cast<tavern::PersonId>(pair[1] + 1));
    const auto friendship = tavern::are_friends(
        database, static_cast<tavern::PersonId>(pair[0] + 1),
        static_cast<tavern::PersonId>(pair[1] + 1));
    if (!following || !friendship ||
        *following != model.follows[pair[0]][pair[1]] ||
        *friendship != model.friends[pair[0]][pair[1]])
      return false;
  }
  return true;
}

auto fail(std::size_t index, std::uint64_t state, std::size_t operation,
          std::size_t left, std::size_t right, const tavern::Database& database)
    -> bool {
  std::cerr << "seed=" << seed << " index=" << index << " op=" << operation
            << " left=" << left + 1 << " right=" << right + 1
            << " generation=" << database_impl(database).commit.generation
            << " pages=" << database_impl(database).store.page_count
            << " state=" << state << '\n';
  return false;
}

auto retained_candidates_are_unreachable(const tavern::Database& database)
    -> bool {
  if (!database_impl(database).retained_free_page_pool) return true;
  const auto scan = tavern::scan_reachable_pages(database_impl(database).store);
  if (!scan) return false;
  for (const tavern::PageId id :
       database_impl(database).retained_free_page_pool->candidates) {
    if (id < tavern::first_data_page ||
        id >= database_impl(database).store.page_count ||
        scan->reachable[static_cast<std::size_t>(id)])
      return false;
  }
  return true;
}

auto shadow_candidates_match(const tavern::Database& database,
                             const tavern::ReachabilityScan& scan) -> bool {
  const auto candidates =
      tavern::page_liveness_shadow_candidates_for_test(database);
  if (!candidates || !database_impl(database).page_liveness_shadow)
    return false;
  std::vector<tavern::PageId> expected;
  for (tavern::PageId id = tavern::first_data_page;
       id < database_impl(database).store.page_count; ++id)
    if (!scan.reachable[static_cast<std::size_t>(id)]) expected.push_back(id);
  if (*candidates != expected) return false;
  for (const tavern::PageId id : *candidates)
    if (database_impl(database)
            .page_liveness_shadow
            ->incoming_references[static_cast<std::size_t>(id)] != 0)
      return false;
  return true;
}

}  // namespace

auto main() -> int {
  const auto file = path();
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  if (!database) return 1;
  Model model{};
  std::uint64_t state = seed;
  std::size_t commits = 0;
  std::size_t noops = 0;
  std::size_t rejected = 0;
  std::size_t reopens = 0;
  std::size_t reuse_events = 0;
  std::size_t append_events = 0;
  std::size_t exhaustion_rebuild_cycles = 0;
  std::size_t maximum_pool_size = 0;
  std::size_t failed_invalidations = 0;
  std::size_t candidate_comparisons = 0;
  std::size_t candidate_mismatches = 0;
  std::size_t allocation_failures = 0;
  bool awaiting_rebuild_after_exhaustion = false;
  std::unordered_set<tavern::PageId> consumed_in_pool_generation;
  const tavern::PageId initial_pages =
      database_impl(*database).store.page_count;
  tavern::reset_free_page_pool_scan_count_for_test();

  for (std::size_t index = 0; index < operations; ++index) {
    const std::size_t kind = static_cast<std::size_t>(next(state) % 4U);
    const std::size_t left = static_cast<std::size_t>(next(state) % population);
    const std::size_t right =
        static_cast<std::size_t>(next(state) % population);
    const tavern::PersonId a = static_cast<tavern::PersonId>(left + 1);
    const tavern::PersonId b = static_cast<tavern::PersonId>(right + 1);
    const auto generation = database_impl(*database).commit.generation;
    const auto pages = database_impl(*database).store.page_count;
    const auto scans_before = tavern::free_page_pool_scan_count_for_test();
    const std::vector<tavern::PageId> pool_before =
        database_impl(*database).retained_free_page_pool
            ? database_impl(*database).retained_free_page_pool->candidates
            : std::vector<tavern::PageId>{};
    std::expected<void, tavern::Error> result =
        std::unexpected(tavern::Error::invalid_page);
    if (kind == 0) result = tavern::follow(*database, a, b);
    if (kind == 1) result = tavern::unfollow(*database, a, b);
    if (kind == 2) result = tavern::befriend(*database, a, b);
    if (kind == 3) result = tavern::unfriend(*database, a, b);
    if (left == right) {
      if (result || result.error() != tavern::Error::self_relationship)
        return fail(index, state, kind, left, right, *database) ? 0 : 1;
      ++rejected;
    } else {
      if (!result)
        return fail(index, state, kind, left, right, *database) ? 0 : 1;
      if (kind == 0) model.follows[left][right] = true;
      if (kind == 1) model.follows[left][right] = false;
      if (kind == 2) {
        model.friends[left][right] = true;
        model.friends[right][left] = true;
      }
      if (kind == 3) {
        model.friends[left][right] = false;
        model.friends[right][left] = false;
      }
      if (database_impl(*database).commit.generation == generation)
        ++noops;
      else {
        ++commits;
        if (database_impl(*database).store.page_count == pages)
          ++reuse_events;
        else
          ++append_events;
        const std::vector<tavern::PageId> pool_after =
            database_impl(*database).retained_free_page_pool
                ? database_impl(*database).retained_free_page_pool->candidates
                : std::vector<tavern::PageId>{};
        const auto scans_after = tavern::free_page_pool_scan_count_for_test();
        if (scans_after > scans_before) {
          consumed_in_pool_generation.clear();
          if (awaiting_rebuild_after_exhaustion) {
            ++exhaustion_rebuild_cycles;
            awaiting_rebuild_after_exhaustion = false;
          }
          if (!retained_candidates_are_unreachable(*database))
            return fail(index, state, kind, left, right, *database) ? 0 : 1;
        }
        if (!pool_before.empty()) {
          if (pool_after.size() > pool_before.size() ||
              !std::equal(pool_after.begin(), pool_after.end(),
                          pool_before.end() -
                              static_cast<std::ptrdiff_t>(pool_after.size())))
            return fail(index, state, kind, left, right, *database) ? 0 : 1;
          for (std::size_t page = 0;
               page < pool_before.size() - pool_after.size(); ++page) {
            if (!consumed_in_pool_generation.insert(pool_before[page]).second)
              return fail(index, state, kind, left, right, *database) ? 0 : 1;
          }
          if (pool_after.empty()) awaiting_rebuild_after_exhaustion = true;
        }
        maximum_pool_size = std::max(maximum_pool_size, pool_after.size());
      }
      if (!matches_affected(*database, model, left, right))
        return fail(index, state, kind, left, right, *database) ? 0 : 1;
    }
    if (index == 249) {
      const auto pool = tavern::build_free_page_pool(*database);
      if (!pool || pool->candidates.empty()) return 1;
      std::size_t failure_left = 0;
      std::size_t failure_right = 1;
      bool found = false;
      for (std::size_t left_index = 0; left_index < population && !found;
           ++left_index)
        for (std::size_t right_index = 0; right_index < population;
             ++right_index)
          if (left_index != right_index &&
              !model.follows[left_index][right_index]) {
            failure_left = left_index;
            failure_right = right_index;
            found = true;
            break;
          }
      if (!found) return 1;
      database_impl(*database).store.fail_reusable_write_after_for_test = 1;
      const auto failed = tavern::follow(
          *database, static_cast<tavern::PersonId>(failure_left + 1),
          static_cast<tavern::PersonId>(failure_right + 1));
      if (failed) return 1;
      if (!tavern::close_database(*database)) return 1;
      database = tavern::open_database(file);
      if (!database || !matches(*database, model)) return 1;
      ++allocation_failures;
      ++reopens;
    }
    if (index == 499) {
      std::size_t failure_left = 0;
      std::size_t failure_right = 1;
      bool add = true;
      bool found = false;
      for (std::size_t left_index = 0; left_index < population && !found;
           ++left_index)
        for (std::size_t right_index = 0; right_index < population;
             ++right_index) {
          if (left_index == right_index) continue;
          if (!model.follows[left_index][right_index]) {
            failure_left = left_index;
            failure_right = right_index;
            add = true;
            found = true;
            break;
          }
          if (model.follows[left_index][right_index]) {
            failure_left = left_index;
            failure_right = right_index;
            add = false;
            found = true;
            break;
          }
        }
      if (!found) return 1;
      database_impl(*database).store.fail_metadata_write_for_test = true;
      const auto failed =
          add ? tavern::follow(*database,
                               static_cast<tavern::PersonId>(failure_left + 1),
                               static_cast<tavern::PersonId>(failure_right + 1))
              : tavern::unfollow(
                    *database, static_cast<tavern::PersonId>(failure_left + 1),
                    static_cast<tavern::PersonId>(failure_right + 1));
      if (failed || !database_impl(*database).commit.recovery_required ||
          database_impl(*database).retained_free_page_pool)
        return 1;
      if (!tavern::close_database(*database)) return 1;
      database = tavern::open_database(file);
      if (!database || database_impl(*database).retained_free_page_pool ||
          !matches(*database, model))
        return 1;
      awaiting_rebuild_after_exhaustion = false;
      ++failed_invalidations;
      ++reopens;
    }
    if ((index + 1) % checkpoint_interval == 0) {
      if (!tavern::close_database(*database)) return 1;
      database = tavern::open_database(file);
      if (!database || database_impl(*database).retained_free_page_pool ||
          !matches(*database, model))
        return 1;
      const auto scan =
          tavern::scan_reachable_pages(database_impl(*database).store);
      const auto roots =
          tavern::scan_protected_directories(database_impl(*database).store);
      if (!scan || !roots || roots->roots.size() != 2 ||
          !tavern::initialize_page_liveness_shadow_for_test(*database))
        return 1;
      ++candidate_comparisons;
      if (!shadow_candidates_match(*database, *scan)) {
        ++candidate_mismatches;
        return 1;
      }
      awaiting_rebuild_after_exhaustion = false;
      ++reopens;
    }
  }
  const tavern::PageId final_pages = database_impl(*database).store.page_count;
  const bool closed = tavern::close_database(*database).has_value();
  std::filesystem::remove(file);
  std::cout << "operations=" << operations << " commits=" << commits
            << " noops=" << noops << " rejected=" << rejected
            << " reopens=" << reopens
            << " scans=" << tavern::free_page_pool_scan_count_for_test()
            << " exhaustion_rebuild_cycles=" << exhaustion_rebuild_cycles
            << " reuse=" << reuse_events << " append=" << append_events
            << " maximum_pool_size=" << maximum_pool_size
            << " failed_invalidations=" << failed_invalidations
            << " allocation_failures=" << allocation_failures
            << " candidate_comparisons=" << candidate_comparisons
            << " candidate_mismatches=" << candidate_mismatches
            << " initial_pages=" << initial_pages
            << " final_pages=" << final_pages << '\n';
  return closed && reopens >= 100 && exhaustion_rebuild_cycles >= 3 &&
                 reuse_events > 1 && append_events > 0 &&
                 failed_invalidations == 1 && allocation_failures == 1 &&
                 candidate_comparisons >= operations / checkpoint_interval &&
                 candidate_mismatches == 0
             ? 0
             : 1;
}
