#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <vector>

#include "adjacency_tree.hpp"
#include "database_internal.hpp"
#include "directory.hpp"
#include "free_page_pool.hpp"
#include "reachability.hpp"

namespace {

constexpr std::size_t operations = 5000;
constexpr std::size_t checkpoint_interval = 1000;
constexpr std::uint64_t seed = 0x95B300C0FFEEULL;

struct Fixture {
  std::size_t relationships;
  std::size_t population;
};

struct Model {
  explicit Model(std::size_t count)
      : population(count), follows(count * count) {}
  auto follow(std::size_t left, std::size_t right) -> std::uint8_t& {
    return follows[left * population + right];
  }
  auto follow(std::size_t left, std::size_t right) const -> bool {
    return follows[left * population + right];
  }
  std::size_t population;
  std::vector<std::uint8_t> follows;
};

struct Operation {
  std::size_t left;
  std::size_t right;
};

auto next(std::uint64_t& state) -> std::uint64_t {
  state ^= state << 7U;
  state ^= state >> 9U;
  state ^= state << 8U;
  return state;
}

auto microseconds(std::chrono::nanoseconds elapsed) -> double {
  return std::chrono::duration<double, std::micro>(elapsed).count();
}

auto percentile(std::vector<double> values, std::size_t percent) -> double {
  std::sort(values.begin(), values.end());
  return values[(values.size() * percent + 99U) / 100U - 1U];
}

auto build_fixture(tavern::Database& database, Model& model,
                   const Fixture& fixture) -> bool {
  std::uint64_t state = seed ^ fixture.relationships;
  std::size_t count = 0;
  while (count < fixture.relationships) {
    const auto left =
        static_cast<std::size_t>(next(state) % fixture.population);
    const auto right =
        static_cast<std::size_t>(next(state) % fixture.population);
    if (left == right || model.follow(left, right)) continue;
    model.follow(left, right) = true;
    ++count;
  }
  tavern::PageId root = database_impl(database).commit.root;
  for (std::size_t left = 0; left < fixture.population; ++left) {
    std::vector<tavern::PersonId> following;
    std::vector<tavern::PersonId> followers;
    for (std::size_t right = 0; right < fixture.population; ++right) {
      if (model.follow(left, right))
        following.push_back(static_cast<tavern::PersonId>(right + 1U));
      if (model.follow(right, left))
        followers.push_back(static_cast<tavern::PersonId>(right + 1U));
    }
    const auto outgoing =
        tavern::build_adjacency_tree(database_impl(database).store, following);
    const auto incoming =
        tavern::build_adjacency_tree(database_impl(database).store, followers);
    if (!outgoing || !incoming) return false;
    tavern::PatronRecord record{};
    record.id = static_cast<tavern::PersonId>(left + 1U);
    record.following = *outgoing;
    record.followers = *incoming;
    const auto inserted =
        tavern::directory_insert(database_impl(database).store, root, record);
    if (!inserted) return false;
    root = *inserted;
  }
  return tavern::commit_root(database_impl(database).store,
                             database_impl(database).commit, root)
      .has_value();
}

auto make_workload(Model model) -> std::vector<Operation> {
  std::vector<Operation> result;
  result.reserve(operations);
  std::uint64_t state = seed ^ 0xB3B3B3B3ULL;
  while (result.size() < operations) {
    const auto left = static_cast<std::size_t>(next(state) % model.population);
    const auto right = static_cast<std::size_t>(next(state) % model.population);
    if (left == right || model.follow(left, right)) continue;
    model.follow(left, right) = true;
    result.push_back({left, right});
  }
  return result;
}

auto verify(const tavern::Database& database, const Model& model,
            bool index_backed) -> bool {
  for (std::size_t left = 0; left < model.population; ++left) {
    std::vector<tavern::PersonId> actual;
    const auto listed = tavern::following(
        database, static_cast<tavern::PersonId>(left + 1U),
        [&](tavern::PersonId id) -> std::expected<void, tavern::Error> {
          actual.push_back(id);
          return std::expected<void, tavern::Error>{};
        });
    if (!listed) return false;
    std::vector<tavern::PersonId> expected;
    for (std::size_t right = 0; right < model.population; ++right)
      if (model.follow(left, right))
        expected.push_back(static_cast<tavern::PersonId>(right + 1U));
    std::sort(actual.begin(), actual.end());
    if (actual != expected) return false;
  }
  const auto roots =
      tavern::scan_protected_directories(database_impl(database).store);
  const auto scan = tavern::scan_reachable_pages(database_impl(database).store);
  if (!roots || roots->roots.size() != 2 || !scan) return false;
  if (!index_backed) return true;
  const auto candidates =
      tavern::page_liveness_shadow_candidates_for_test(database);
  if (!candidates) return false;
  std::vector<tavern::PageId> expected;
  for (tavern::PageId page = tavern::first_data_page;
       page < database_impl(database).store.page_count; ++page)
    if (!scan->reachable[static_cast<std::size_t>(page)])
      expected.push_back(page);
  return *candidates == expected;
}

auto run(const Fixture& fixture, bool index_backed) -> bool {
  const auto file =
      std::filesystem::temp_directory_path() /
      ("tavern-reclamation-" + std::to_string(fixture.relationships) +
       (index_backed ? "-index.db" : "-pool.db"));
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  Model model(fixture.population);
  if (!database || !build_fixture(*database, model, fixture)) return false;
  const auto workload = make_workload(model);
  const tavern::PageId initial_pages =
      database_impl(*database).store.page_count;
  std::chrono::nanoseconds reconstruction{};
  if (index_backed) {
    const auto started = std::chrono::steady_clock::now();
    if (!tavern::initialize_page_liveness_shadow_for_test(*database))
      return false;
    reconstruction = std::chrono::steady_clock::now() - started;
  }
  tavern::reset_free_page_pool_scan_count_for_test();
  tavern::reset_reachability_scan_count_for_test();
  std::vector<double> latency;
  std::chrono::nanoseconds total{};
  std::chrono::nanoseconds updates{};
  std::chrono::nanoseconds registration{};
  std::chrono::nanoseconds activation{};
  std::chrono::nanoseconds deactivation{};
  std::chrono::nanoseconds candidate_refresh{};
  std::size_t reused = 0;
  std::size_t appended = 0;
  std::size_t commits = 0;
  for (std::size_t index = 0; index < workload.size(); ++index) {
    const auto operation = workload[index];
    const auto started = std::chrono::steady_clock::now();
    const auto result =
        index_backed
            ? tavern::follow_with_index_reuse_for_test(
                  *database, static_cast<tavern::PersonId>(operation.left + 1U),
                  static_cast<tavern::PersonId>(operation.right + 1U))
            : tavern::follow(
                  *database, static_cast<tavern::PersonId>(operation.left + 1U),
                  static_cast<tavern::PersonId>(operation.right + 1U));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    if (!result) return false;
    model.follow(operation.left, operation.right) = true;
    latency.push_back(microseconds(elapsed));
    total += elapsed;
    ++commits;
    const auto& diagnostics =
        database_impl(*database).page_liveness_shadow_diagnostics;
    reused += diagnostics.reused_pages;
    appended += diagnostics.appended_pages;
    if (index_backed) {
      updates += diagnostics.elapsed;
      registration += diagnostics.registration_elapsed;
      activation += diagnostics.activation_elapsed;
      deactivation += diagnostics.deactivation_elapsed;
      candidate_refresh += diagnostics.candidate_refresh_elapsed;
    }
    if ((index + 1U) % checkpoint_interval == 0U &&
        !verify(*database, model, index_backed)) {
      return false;
    }
  }
  const auto scan_count = tavern::free_page_pool_scan_count_for_test();
  const auto scan_time = tavern::free_page_pool_scan_elapsed_for_test();
  const tavern::PageId final_pages = database_impl(*database).store.page_count;
  if (!verify(*database, model, index_backed) ||
      !tavern::close_database(*database))
    return false;
  auto reopened = tavern::open_database(file);
  const bool durable =
      reopened &&
      (!index_backed ||
       tavern::initialize_page_liveness_shadow_for_test(*reopened)) &&
      verify(*reopened, model, index_backed) &&
      tavern::close_database(*reopened);
  std::filesystem::remove(file);
  if (!durable) return false;
  std::cout << "mode=" << (index_backed ? "index" : "pool")
            << " relationships=" << fixture.relationships
            << " attempts=" << workload.size() << " commits=" << commits
            << " noops=0 measured_total_us=" << microseconds(total)
            << " ops_per_second="
            << static_cast<double>(workload.size()) * 1000000.0 /
                   microseconds(total)
            << " initial_reconstruction_us=" << microseconds(reconstruction)
            << " initialization_inclusive_us="
            << microseconds(total + reconstruction)
            << " full_scan_count=" << scan_count
            << " full_scan_total_us=" << microseconds(scan_time)
            << " index_update_total_us=" << microseconds(updates)
            << " registration_us=" << microseconds(registration)
            << " activation_us=" << microseconds(activation)
            << " deactivation_us=" << microseconds(deactivation)
            << " candidate_refresh_us=" << microseconds(candidate_refresh)
            << " other_index_us="
            << microseconds(updates - registration - activation - deactivation -
                            candidate_refresh)
            << " reused_pages=" << reused << " appended_pages=" << appended
            << " initial_pages=" << initial_pages
            << " final_pages=" << final_pages
            << " physical_growth=" << final_pages - initial_pages
            << " mutation_p50_us=" << percentile(latency, 50)
            << " mutation_p95_us=" << percentile(latency, 95)
            << " mutation_p99_us=" << percentile(latency, 99) << '\n';
  return true;
}

}  // namespace

auto main() -> int {
  std::cout << std::fixed << std::setprecision(2);
  for (const Fixture fixture :
       {Fixture{1000, 128}, Fixture{10000, 512}, Fixture{100000, 2048}})
    if (!run(fixture, false) || !run(fixture, true)) return 1;
  return 0;
}
