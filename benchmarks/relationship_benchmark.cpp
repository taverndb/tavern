#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "adjacency_tree.hpp"
#include "database_internal.hpp"
#include "directory.hpp"
#include "free_page_pool.hpp"
#include "reachability.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using Samples = std::vector<double>;

constexpr std::size_t operations = 5000;
constexpr std::size_t mixed_operations = 10000;
constexpr std::size_t mixed_warmup_operations = 500;
constexpr std::size_t mixed_population = 96;
constexpr std::uint64_t seed = 0x6D362D776F726B6CULL;

enum class Workload { sparse, dense, insertion_heavy, churn_heavy };

struct Stats {
  double median = 0;
  double p95 = 0;
  double p99 = 0;
};

struct Model {
  explicit Model(std::size_t count)
      : population(count), follows(count * count), friends(count * count) {}

  auto follow(std::size_t left, std::size_t right) -> std::uint8_t& {
    return follows[left * population + right];
  }
  auto friend_of(std::size_t left, std::size_t right) -> std::uint8_t& {
    return friends[left * population + right];
  }

  std::size_t population;
  std::vector<std::uint8_t> follows;
  std::vector<std::uint8_t> friends;
};

struct Measurements {
  std::size_t commits = 0;
  std::size_t noops = 0;
  std::size_t rejected = 0;
  std::size_t scans = 0;
  std::size_t rebuilds = 0;
  std::size_t reuse_commits = 0;
  std::size_t append_commits = 0;
  tavern::PageId appended_pages = 0;
  std::size_t candidates_consumed = 0;
  std::size_t maximum_pool_size = 0;
  Samples mutations;
  Samples warm_mutations;
  Samples cold_mutations;
  Samples pool_sizes_after_rebuild;
  Samples commits_between_rebuilds;
};

auto stats(Samples values) -> Stats {
  std::sort(values.begin(), values.end());
  const std::size_t median_index = values.size() / 2;
  const std::size_t p95_index = (values.size() * 95U + 99U) / 100U - 1U;
  const std::size_t p99_index = (values.size() * 99U + 99U) / 100U - 1U;
  return {values[median_index], values[p95_index], values[p99_index]};
}

auto microseconds(std::chrono::nanoseconds duration) -> double {
  return std::chrono::duration<double, std::micro>(duration).count();
}

void print_stats(const char* name, const Samples& values) {
  if (values.empty()) {
    std::cout << ' ' << name << "_median_us=na " << name << "_p95_us=na "
              << name << "_p99_us=na";
    return;
  }
  const auto value = stats(values);
  std::cout << ' ' << name << "_median_us=" << value.median << ' ' << name
            << "_p95_us=" << value.p95 << ' ' << name
            << "_p99_us=" << value.p99;
}

void print_distribution(const char* name, const Samples& values) {
  if (values.empty()) {
    std::cout << ' ' << name << "_median=na " << name << "_p95=na";
    return;
  }
  const auto value = stats(values);
  std::cout << ' ' << name << "_median=" << value.median << ' ' << name
            << "_p95=" << value.p95;
}

auto next(std::uint64_t& state) -> std::uint64_t {
  state ^= state << 7U;
  state ^= state >> 9U;
  state ^= state << 8U;
  return state;
}

auto name(Workload workload) -> const char* {
  switch (workload) {
    case Workload::sparse:
      return "sparse";
    case Workload::dense:
      return "dense";
    case Workload::insertion_heavy:
      return "insertion-heavy";
    case Workload::churn_heavy:
      return "churn-heavy";
  }
  return "unknown";
}

auto population(Workload workload) -> std::size_t {
  switch (workload) {
    case Workload::sparse:
      return 512;
    case Workload::dense:
      return 24;
    case Workload::insertion_heavy:
      return 96;
    case Workload::churn_heavy:
      return 16;
  }
  return 0;
}

auto mutation_kind(Workload workload, std::uint64_t random) -> std::size_t {
  if (workload == Workload::insertion_heavy) {
    if (random % 10U < 8U) return static_cast<std::size_t>(random % 2U) * 2U;
    return static_cast<std::size_t>(random % 2U) * 2U + 1U;
  }
  if (workload == Workload::churn_heavy) {
    return static_cast<std::size_t>(random % 4U);
  }
  return static_cast<std::size_t>(random % 4U);
}

auto path(Workload workload) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         ("tavern-workload-" + std::string(name(workload)) + "-" +
          std::to_string(static_cast<long long>(getpid())) + ".db");
}

auto matches_affected(const tavern::Database& database, const Model& model,
                      std::size_t left, std::size_t right) -> bool {
  for (const auto pair : {std::array<std::size_t, 2>{left, right},
                          std::array<std::size_t, 2>{right, left}}) {
    const auto following = tavern::is_following(
        database, static_cast<tavern::PersonId>(pair[0] + 1U),
        static_cast<tavern::PersonId>(pair[1] + 1U));
    const auto friendship = tavern::are_friends(
        database, static_cast<tavern::PersonId>(pair[0] + 1U),
        static_cast<tavern::PersonId>(pair[1] + 1U));
    if (!following || !friendship ||
        *following != model.follows[pair[0] * model.population + pair[1]] ||
        *friendship != model.friends[pair[0] * model.population + pair[1]]) {
      return false;
    }
  }
  return true;
}

auto matches_checkpoint(const tavern::Database& database, const Model& model,
                        std::uint64_t state) -> bool {
  for (std::size_t index = 0; index < 256; ++index) {
    const std::size_t left =
        static_cast<std::size_t>(next(state) % model.population);
    const std::size_t right =
        static_cast<std::size_t>(next(state) % model.population);
    if (!matches_affected(database, model, left, right)) return false;
  }
  return true;
}

auto run(Workload workload) -> bool {
  const auto file = path(workload);
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  if (!database) return false;
  Model model(population(workload));
  Measurements measurements;
  const tavern::PageId initial_pages =
      database_impl(*database).store.page_count;
  std::uint64_t state = seed ^ static_cast<std::uint64_t>(workload);
  std::uint64_t checkpoint_state = state ^ 0x9E3779B97F4A7C15ULL;
  std::size_t commits_since_rebuild = 0;
  tavern::reset_free_page_pool_scan_count_for_test();

  for (std::size_t index = 0; index < operations; ++index) {
    const std::size_t kind = mutation_kind(workload, next(state));
    const std::size_t left =
        static_cast<std::size_t>(next(state) % model.population);
    const std::size_t right =
        static_cast<std::size_t>(next(state) % model.population);
    const tavern::PersonId a = static_cast<tavern::PersonId>(left + 1U);
    const tavern::PersonId b = static_cast<tavern::PersonId>(right + 1U);
    const auto generation = database_impl(*database).commit.generation;
    const auto pages = database_impl(*database).store.page_count;
    const auto scans_before = tavern::free_page_pool_scan_count_for_test();
    const std::vector<tavern::PageId> pool_before =
        database_impl(*database).retained_free_page_pool
            ? database_impl(*database).retained_free_page_pool->candidates
            : std::vector<tavern::PageId>{};
    const auto start = Clock::now();
    std::expected<void, tavern::Error> result =
        std::unexpected(tavern::Error::invalid_page);
    if (kind == 0) result = tavern::follow(*database, a, b);
    if (kind == 1) result = tavern::unfollow(*database, a, b);
    if (kind == 2) result = tavern::befriend(*database, a, b);
    if (kind == 3) result = tavern::unfriend(*database, a, b);
    const double elapsed =
        std::chrono::duration<double, std::micro>(Clock::now() - start).count();
    if (left == right) {
      if (result || result.error() != tavern::Error::self_relationship)
        return false;
      ++measurements.rejected;
      continue;
    }
    if (!result) return false;
    if (kind == 0) model.follow(left, right) = true;
    if (kind == 1) model.follow(left, right) = false;
    if (kind == 2) {
      model.friend_of(left, right) = true;
      model.friend_of(right, left) = true;
    }
    if (kind == 3) {
      model.friend_of(left, right) = false;
      model.friend_of(right, left) = false;
    }
    if (!matches_affected(*database, model, left, right)) return false;
    if (database_impl(*database).commit.generation == generation) {
      ++measurements.noops;
    } else {
      ++measurements.commits;
      ++commits_since_rebuild;
      measurements.mutations.push_back(elapsed);
      const auto scans_after = tavern::free_page_pool_scan_count_for_test();
      const bool rebuilt = scans_after > scans_before;
      if (rebuilt) {
        ++measurements.rebuilds;
        measurements.commits_between_rebuilds.push_back(
            static_cast<double>(commits_since_rebuild - 1U));
        commits_since_rebuild = 0;
        const std::size_t pool_size =
            database_impl(*database).retained_free_page_pool
                ? database_impl(*database)
                      .retained_free_page_pool->candidates.size()
                : 0;
        measurements.candidates_consumed +=
            tavern::last_free_page_pool_candidate_count_for_test() - pool_size;
        measurements.pool_sizes_after_rebuild.push_back(
            static_cast<double>(pool_size));
        measurements.cold_mutations.push_back(elapsed);
      } else {
        measurements.warm_mutations.push_back(elapsed);
      }
      if (database_impl(*database).store.page_count == pages) {
        ++measurements.reuse_commits;
      } else {
        ++measurements.append_commits;
        measurements.appended_pages +=
            database_impl(*database).store.page_count - pages;
      }
      if (!pool_before.empty() &&
          database_impl(*database).retained_free_page_pool) {
        const auto pool_after =
            database_impl(*database).retained_free_page_pool->candidates.size();
        if (pool_after > pool_before.size()) return false;
        if (!rebuilt)
          measurements.candidates_consumed += pool_before.size() - pool_after;
      } else if (!pool_before.empty()) {
        if (!rebuilt) measurements.candidates_consumed += pool_before.size();
      }
      const std::size_t pool_size =
          database_impl(*database).retained_free_page_pool
              ? database_impl(*database)
                    .retained_free_page_pool->candidates.size()
              : 0;
      measurements.maximum_pool_size =
          std::max(measurements.maximum_pool_size, pool_size);
    }
    if ((index + 1U) % 500U == 0U &&
        !matches_checkpoint(*database, model, checkpoint_state)) {
      return false;
    }
  }

  Samples scan_samples;
  for (std::size_t index = 0; index < 5; ++index) {
    const auto start = Clock::now();
    if (!tavern::scan_reachable_pages(database_impl(*database).store))
      return false;
    scan_samples.push_back(
        std::chrono::duration<double, std::micro>(Clock::now() - start)
            .count());
  }
  const tavern::PageId final_pages = database_impl(*database).store.page_count;
  measurements.scans = tavern::free_page_pool_scan_count_for_test();
  const bool closed = tavern::close_database(*database).has_value();
  std::filesystem::remove(file);
  if (!closed || measurements.mutations.empty() || scan_samples.empty())
    return false;

  std::cout << "workload=" << name(workload) << " seed=" << seed
            << " operations=" << operations
            << " commits=" << measurements.commits
            << " noops=" << measurements.noops
            << " rejected=" << measurements.rejected
            << " scans=" << measurements.scans << " scan_per_commit="
            << static_cast<double>(measurements.scans) /
                   static_cast<double>(measurements.commits)
            << " rebuilds=" << measurements.rebuilds
            << " reuse_commits=" << measurements.reuse_commits
            << " append_commits=" << measurements.append_commits
            << " appended_pages=" << measurements.appended_pages
            << " max_pool=" << measurements.maximum_pool_size
            << " candidates_per_commit="
            << static_cast<double>(measurements.candidates_consumed) /
                   static_cast<double>(measurements.commits)
            << " initial_pages=" << initial_pages
            << " final_pages=" << final_pages;
  print_stats("mutation", measurements.mutations);
  print_stats("warm", measurements.warm_mutations);
  print_stats("cold", measurements.cold_mutations);
  print_stats("final_state_scan", scan_samples);
  print_distribution("pool_after_rebuild_pages",
                     measurements.pool_sizes_after_rebuild);
  print_distribution("commits_between_rebuilds",
                     measurements.commits_between_rebuilds);
  std::cout << '\n';
  return true;
}

auto build_large_sparse_fixture(tavern::Database& database) -> bool {
  for (std::size_t index = 0; index < 1000; ++index) {
    if (!tavern::follow(database, static_cast<tavern::PersonId>(index + 1U),
                        static_cast<tavern::PersonId>(100000U + index))) {
      return false;
    }
  }
  return true;
}

auto build_small_dense_fixture(tavern::Database& database) -> bool {
  for (tavern::PersonId left = 1; left <= 24; ++left) {
    for (tavern::PersonId right = 1; right <= 24; ++right) {
      if (left != right && !tavern::follow(database, left, right)) return false;
    }
  }
  return true;
}

auto build_large_unreachable_fixture(tavern::Database& database) -> bool {
  if (!tavern::follow(database, 1, 2)) return false;
  const std::array<std::byte, 1> payload{std::byte{0x6D}};
  for (std::size_t index = 0; index < 10000; ++index) {
    if (!tavern::append_page(database_impl(database).store,
                             tavern::PageType::data, payload))
      return false;
  }
  return true;
}

auto build_cow_sharing_fixture(tavern::Database& database) -> bool {
  for (std::size_t index = 0; index < 400; ++index) {
    if (!tavern::follow(database, static_cast<tavern::PersonId>(index + 1U),
                        static_cast<tavern::PersonId>(200000U + index))) {
      return false;
    }
  }
  return tavern::follow(database, 1, 999999).has_value();
}

using FixtureBuilder = bool (*)(tavern::Database&);

auto profile_fixture(const char* fixture, FixtureBuilder builder) -> bool {
  const auto file = std::filesystem::temp_directory_path() /
                    ("tavern-reclamation-scan-" + std::string(fixture) + "-" +
                     std::to_string(static_cast<long long>(getpid())) + ".db");
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  if (!database || !builder(*database)) return false;

  constexpr std::size_t samples = 7;
  Samples totals;
  Samples bitmap_initialization;
  Samples metadata;
  Samples directory;
  Samples adjacency;
  Samples marking;
  Samples counting;
  Samples extraction;
  tavern::ReachabilityScanDiagnostics diagnostics;
  for (std::size_t index = 0; index < samples; ++index) {
    const auto profile =
        tavern::profile_reachable_pages(database_impl(*database).store);
    if (!profile) return false;
    diagnostics = profile->diagnostics;
    totals.push_back(microseconds(diagnostics.total));
    bitmap_initialization.push_back(
        microseconds(diagnostics.bitmap_initialization));
    metadata.push_back(microseconds(diagnostics.metadata_inspection));
    directory.push_back(microseconds(diagnostics.directory_traversal));
    adjacency.push_back(microseconds(diagnostics.adjacency_traversal));
    marking.push_back(microseconds(diagnostics.bitmap_marking));
    counting.push_back(microseconds(diagnostics.bitmap_counting));
    const auto extraction_start = Clock::now();
    std::vector<tavern::PageId> candidates;
    candidates.reserve(
        static_cast<std::size_t>(diagnostics.unreachable_data_pages));
    for (tavern::PageId id = tavern::first_data_page;
         id < database_impl(*database).store.page_count; ++id) {
      if (!profile->scan.reachable[static_cast<std::size_t>(id)])
        candidates.push_back(id);
    }
    extraction.push_back(std::chrono::duration<double, std::micro>(
                             Clock::now() - extraction_start)
                             .count());
    if (candidates.size() != diagnostics.unreachable_data_pages) return false;
  }
  const bool closed = tavern::close_database(*database).has_value();
  std::filesystem::remove(file);
  if (!closed) return false;

  const auto total = stats(totals);
  const auto percentage = [&](const Samples& values) -> double {
    return total.median == 0.0 ? 0.0
                               : stats(values).median * 100.0 / total.median;
  };
  std::cout << "fixture=" << fixture
            << " physical_pages=" << diagnostics.physical_data_pages
            << " reachable_pages=" << diagnostics.reachable_data_pages
            << " unreachable_pages=" << diagnostics.unreachable_data_pages
            << " directory_visits=" << diagnostics.directory_page_visits
            << " directory_unique=" << diagnostics.directory_unique_pages
            << " adjacency_visits=" << diagnostics.adjacency_page_visits
            << " adjacency_unique=" << diagnostics.adjacency_unique_pages
            << " repeated_visits=" << diagnostics.repeated_page_visits
            << " memo_hits=" << diagnostics.adjacency_memo_hits
            << " memo_extent_relaxed_hits="
            << diagnostics.adjacency_memo_extent_relaxed_hits
            << " memo_skipped_visits="
            << diagnostics.adjacency_memo_skipped_page_visits
            << " memo_entries=" << diagnostics.adjacency_memo_entries
            << " memo_payload_bytes="
            << diagnostics.adjacency_memo_payload_bytes
            << " logical_page_reads=" << diagnostics.logical_page_reads;
  print_stats("scan_total", totals);
  print_stats("bitmap_init", bitmap_initialization);
  print_stats("metadata", metadata);
  print_stats("directory", directory);
  print_stats("adjacency", adjacency);
  print_stats("marking", marking);
  print_stats("counting", counting);
  print_stats("candidate_extract", extraction);
  std::cout << " metadata_pct=" << percentage(metadata)
            << " directory_pct=" << percentage(directory)
            << " adjacency_pct=" << percentage(adjacency) << " bitmap_pct="
            << percentage(bitmap_initialization) + percentage(marking) +
                   percentage(counting)
            << " candidate_extract_pct=" << percentage(extraction) << '\n';
  return true;
}

auto build_degree_fixture(tavern::Database& database, std::size_t degree,
                          std::vector<tavern::PersonId>& targets) -> bool {
  targets.reserve(degree);
  for (std::size_t index = 0; index < degree; ++index)
    targets.push_back(static_cast<tavern::PersonId>(10000U + index));
  const std::array<tavern::PersonId, 1> follower{1};
  const std::array<tavern::PersonId, 2> friend_hubs{2, 3};
  const auto following =
      tavern::build_adjacency_tree(database_impl(database).store, targets);
  const auto friends =
      tavern::build_adjacency_tree(database_impl(database).store, targets);
  const auto target_followers =
      tavern::build_adjacency_tree(database_impl(database).store, follower);
  const auto target_friends =
      tavern::build_adjacency_tree(database_impl(database).store, friend_hubs);
  if (!following || !friends || !target_followers || !target_friends)
    return false;

  tavern::PageId root = database_impl(database).commit.root;
  const auto insert = [&](const tavern::PatronRecord& record) -> bool {
    const auto candidate =
        tavern::directory_insert(database_impl(database).store, root, record);
    if (!candidate) return false;
    root = *candidate;
    return true;
  };
  tavern::PatronRecord follow_hub{};
  follow_hub.id = 1;
  follow_hub.following = *following;
  tavern::PatronRecord first_friend_hub{};
  first_friend_hub.id = 2;
  first_friend_hub.friends = *friends;
  tavern::PatronRecord second_friend_hub{};
  second_friend_hub.id = 3;
  second_friend_hub.friends = *friends;
  if (!insert(follow_hub) || !insert(first_friend_hub) ||
      !insert(second_friend_hub))
    return false;
  for (const tavern::PersonId id : targets) {
    tavern::PatronRecord target{};
    target.id = id;
    target.followers = *target_followers;
    target.friends = *target_friends;
    if (!insert(target)) return false;
  }
  return tavern::commit_root(database_impl(database).store,
                             database_impl(database).commit, root)
      .has_value();
}

template <typename Operation>
auto profile_read(tavern::Database& database, std::size_t degree,
                  const char* operation, Operation callback) -> bool {
  constexpr std::size_t samples = 31;
  Samples latency;
  Samples logical_reads;
  Samples unique_pages;
  latency.reserve(samples);
  logical_reads.reserve(samples);
  unique_pages.reserve(samples);
  for (std::size_t index = 0; index < samples; ++index) {
    tavern::begin_page_read_tracking_for_test(database_impl(database).store);
    const auto start = Clock::now();
    if (!callback()) return false;
    latency.push_back(
        std::chrono::duration<double, std::micro>(Clock::now() - start)
            .count());
    const auto reads =
        tavern::end_page_read_tracking_for_test(database_impl(database).store);
    logical_reads.push_back(static_cast<double>(reads.logical_reads));
    unique_pages.push_back(static_cast<double>(reads.unique_pages));
  }
  std::cout << "read_degree=" << degree << " operation=" << operation;
  print_stats("latency", latency);
  print_distribution("logical_reads", logical_reads);
  print_distribution("unique_pages", unique_pages);
  std::cout << '\n';
  return true;
}

auto profile_degree(std::size_t degree) -> bool {
  const auto file = std::filesystem::temp_directory_path() /
                    ("tavern-degree-" + std::to_string(degree) + "-" +
                     std::to_string(static_cast<long long>(getpid())) + ".db");
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  std::vector<tavern::PersonId> targets;
  if (!database || !build_degree_fixture(*database, degree, targets))
    return false;
  const std::array<std::pair<const char*, tavern::PersonId>, 3> positions{
      std::pair{"begin", targets.front()},
      std::pair{"middle", targets[targets.size() / 2U]},
      std::pair{"end", targets.back()}};
  for (const auto& [position, target] : positions) {
    const std::string follow_hit = std::string("follow_hit_") + position;
    const std::string friend_hit = std::string("friend_hit_") + position;
    if (!profile_read(*database, degree, follow_hit.c_str(),
                      [&] -> bool {
                        const auto result =
                            tavern::is_following(*database, 1, target);
                        return result && *result;
                      }) ||
        !profile_read(*database, degree, friend_hit.c_str(), [&] -> bool {
          const auto result = tavern::are_friends(*database, 2, target);
          return result && *result;
        })) {
      return false;
    }
  }
  const tavern::PersonId missing =
      static_cast<tavern::PersonId>(10000U + degree + 1U);
  if (!profile_read(*database, degree, "follow_missing",
                    [&] -> bool {
                      const auto result =
                          tavern::is_following(*database, 1, missing);
                      return result && !*result;
                    }) ||
      !profile_read(*database, degree, "friend_missing",
                    [&] -> bool {
                      const auto result =
                          tavern::are_friends(*database, 2, missing);
                      return result && !*result;
                    }) ||
      !profile_read(
          *database, degree, "following_enumeration",
          [&] -> bool {
            std::size_t count = 0;
            const auto result = tavern::following(
                *database, 1,
                [&](tavern::PersonId) -> std::expected<void, tavern::Error> {
                  ++count;
                  return std::expected<void, tavern::Error>{};
                });
            return result && count == degree;
          }) ||
      !profile_read(*database, degree, "mutual_friends", [&] -> bool {
        const auto result = tavern::mutual_friends(*database, 2, 3);
        return result && result->size() == degree;
      })) {
    return false;
  }
  std::cout << "read_degree=" << degree
            << " operation=mutual_friends_details result_cardinality=" << degree
            << " temporary_hash_members=" << degree << '\n';
  const bool closed = tavern::close_database(*database).has_value();
  std::filesystem::remove(file);
  return closed;
}

enum class MixedWorkload { read_heavy, balanced, write_heavy };

auto name(MixedWorkload workload) -> const char* {
  switch (workload) {
    case MixedWorkload::read_heavy:
      return "read-heavy";
    case MixedWorkload::balanced:
      return "balanced";
    case MixedWorkload::write_heavy:
      return "write-heavy";
  }
  return "unknown";
}

auto read_percent(MixedWorkload workload) -> std::size_t {
  switch (workload) {
    case MixedWorkload::read_heavy:
      return 95;
    case MixedWorkload::balanced:
      return 50;
    case MixedWorkload::write_heavy:
      return 20;
  }
  return 0;
}

struct MixedMeasurements {
  std::array<std::size_t, 4> read_kinds{};
  std::array<std::size_t, 4> mutation_kinds{};
  std::size_t commits = 0;
  std::size_t noops = 0;
  std::size_t rejected = 0;
  std::size_t reuse_commits = 0;
  std::size_t append_commits = 0;
  tavern::PageId appended_pages = 0;
  std::size_t scans = 0;
  Samples reads;
  Samples membership_reads;
  Samples mutual_reads;
  Samples mutations;
  Samples warm_mutations;
  Samples cold_mutations;
  double operation_us = 0;
  std::uint64_t result_checksum = 0;
};

auto matches_ids(const std::vector<tavern::PersonId>& actual,
                 const std::vector<std::uint8_t>& expected) -> bool {
  if (actual.size() !=
      static_cast<std::size_t>(std::count(expected.begin(), expected.end(), 1)))
    return false;
  std::unordered_set<tavern::PersonId> ids;
  for (const tavern::PersonId id : actual) {
    if (id == 0 || id > expected.size() || !expected[id - 1U] ||
        !ids.insert(id).second)
      return false;
  }
  return true;
}

auto populate_mixed_fixture(tavern::Database& database, Model& model) -> bool {
  for (std::size_t index = 0; index < model.population; ++index) {
    const std::size_t next_id = (index + 1U) % model.population;
    const std::size_t friend_id = (index + 7U) % model.population;
    if (!tavern::follow(database, static_cast<tavern::PersonId>(index + 1U),
                        static_cast<tavern::PersonId>(next_id + 1U)) ||
        !tavern::befriend(database, static_cast<tavern::PersonId>(index + 1U),
                          static_cast<tavern::PersonId>(friend_id + 1U))) {
      return false;
    }
    model.follow(index, next_id) = true;
    model.friend_of(index, friend_id) = true;
    model.friend_of(friend_id, index) = true;
  }
  return true;
}

auto execute_mixed_read(tavern::Database& database, const Model& model,
                        std::size_t kind, std::size_t left, std::size_t right,
                        std::uint64_t& checksum) -> bool {
  const tavern::PersonId a = static_cast<tavern::PersonId>(left + 1U);
  const tavern::PersonId b = static_cast<tavern::PersonId>(right + 1U);
  if (kind == 0) {
    const auto result = tavern::is_following(database, a, b);
    if (!result ||
        *result !=
            static_cast<bool>(model.follows[left * model.population + right]))
      return false;
    checksum += *result ? 1U : 0U;
    return true;
  }
  if (kind == 1) {
    const auto result = tavern::are_friends(database, a, b);
    if (!result ||
        *result !=
            static_cast<bool>(model.friends[left * model.population + right]))
      return false;
    checksum += *result ? 3U : 0U;
    return true;
  }
  if (kind == 2) {
    std::vector<tavern::PersonId> actual;
    const auto result = tavern::following(
        database, a,
        [&](tavern::PersonId id) -> std::expected<void, tavern::Error> {
          actual.push_back(id);
          return std::expected<void, tavern::Error>{};
        });
    std::vector<std::uint8_t> expected(model.population);
    for (std::size_t index = 0; index < model.population; ++index)
      expected[index] = model.follows[left * model.population + index];
    if (!result || !matches_ids(actual, expected)) return false;
    checksum += actual.size() * 5U;
    return true;
  }
  const auto result = tavern::mutual_friends(database, a, b);
  std::vector<std::uint8_t> expected(model.population);
  for (std::size_t index = 0; index < model.population; ++index)
    expected[index] = model.friends[left * model.population + index] &&
                      model.friends[right * model.population + index];
  if (!result || !matches_ids(*result, expected)) return false;
  checksum += result->size() * 7U;
  return true;
}

auto execute_mixed_mutation(tavern::Database& database, Model& model,
                            std::size_t kind, std::size_t left,
                            std::size_t right, MixedMeasurements& measurements,
                            bool measure) -> bool {
  const tavern::PersonId a = static_cast<tavern::PersonId>(left + 1U);
  const tavern::PersonId b = static_cast<tavern::PersonId>(right + 1U);
  const auto generation = database_impl(database).commit.generation;
  const auto pages = database_impl(database).store.page_count;
  const auto scans_before = tavern::free_page_pool_scan_count_for_test();
  const auto start = Clock::now();
  std::expected<void, tavern::Error> result =
      std::unexpected(tavern::Error::invalid_page);
  if (kind == 0) result = tavern::follow(database, a, b);
  if (kind == 1) result = tavern::unfollow(database, a, b);
  if (kind == 2) result = tavern::befriend(database, a, b);
  if (kind == 3) result = tavern::unfriend(database, a, b);
  const double elapsed =
      std::chrono::duration<double, std::micro>(Clock::now() - start).count();
  if (measure) measurements.operation_us += elapsed;
  if (left == right) {
    if (result || result.error() != tavern::Error::self_relationship)
      return false;
    if (measure) ++measurements.rejected;
    return true;
  }
  if (!result) return false;
  if (kind == 0) model.follow(left, right) = true;
  if (kind == 1) model.follow(left, right) = false;
  if (kind == 2) {
    model.friend_of(left, right) = true;
    model.friend_of(right, left) = true;
  }
  if (kind == 3) {
    model.friend_of(left, right) = false;
    model.friend_of(right, left) = false;
  }
  if (!measure) return true;
  if (database_impl(database).commit.generation == generation) {
    ++measurements.noops;
    return true;
  }
  ++measurements.commits;
  measurements.mutations.push_back(elapsed);
  if (tavern::free_page_pool_scan_count_for_test() > scans_before)
    measurements.cold_mutations.push_back(elapsed);
  else
    measurements.warm_mutations.push_back(elapsed);
  if (database_impl(database).store.page_count == pages)
    ++measurements.reuse_commits;
  else {
    ++measurements.append_commits;
    measurements.appended_pages +=
        database_impl(database).store.page_count - pages;
  }
  return true;
}

auto run_mixed(MixedWorkload workload) -> bool {
  const auto file = std::filesystem::temp_directory_path() /
                    ("tavern-mixed-" + std::string(name(workload)) + "-" +
                     std::to_string(static_cast<long long>(getpid())) + ".db");
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  Model model(mixed_population);
  if (!database || !populate_mixed_fixture(*database, model)) return false;
  const tavern::PageId initial_pages =
      database_impl(*database).store.page_count;
  std::uint64_t state =
      seed ^ 0x4D382E31776F726BULL ^ static_cast<std::uint64_t>(workload);
  std::uint64_t checkpoint_state = state ^ 0x9E3779B97F4A7C15ULL;
  MixedMeasurements measurements;
  const auto run_operation = [&](bool measure) -> bool {
    const bool is_read = next(state) % 100U < read_percent(workload);
    const std::size_t kind = static_cast<std::size_t>(next(state) % 4U);
    const std::size_t left =
        static_cast<std::size_t>(next(state) % model.population);
    const std::size_t right =
        static_cast<std::size_t>(next(state) % model.population);
    if (is_read) {
      const auto start = Clock::now();
      const bool valid = execute_mixed_read(*database, model, kind, left, right,
                                            measurements.result_checksum);
      const double elapsed =
          std::chrono::duration<double, std::micro>(Clock::now() - start)
              .count();
      if (measure) {
        ++measurements.read_kinds[kind];
        measurements.reads.push_back(elapsed);
        if (kind < 2U) measurements.membership_reads.push_back(elapsed);
        if (kind == 3U) measurements.mutual_reads.push_back(elapsed);
        measurements.operation_us += elapsed;
      }
      return valid;
    }
    if (measure) ++measurements.mutation_kinds[kind];
    return execute_mixed_mutation(*database, model, kind, left, right,
                                  measurements, measure);
  };
  const auto warmup_start = Clock::now();
  for (std::size_t index = 0; index < mixed_warmup_operations; ++index)
    if (!run_operation(false)) return false;
  const double warmup_us =
      std::chrono::duration<double, std::micro>(Clock::now() - warmup_start)
          .count();
  if (!matches_checkpoint(*database, model, checkpoint_state)) return false;
  tavern::reset_free_page_pool_scan_count_for_test();
  const auto measured_start = Clock::now();
  for (std::size_t index = 0; index < mixed_operations; ++index) {
    if (!run_operation(true)) return false;
    if ((index + 1U) % 500U == 0U &&
        !matches_checkpoint(*database, model, checkpoint_state)) {
      return false;
    }
  }
  const double measured_us =
      std::chrono::duration<double, std::micro>(Clock::now() - measured_start)
          .count();
  measurements.scans = tavern::free_page_pool_scan_count_for_test();
  const tavern::PageId final_pages = database_impl(*database).store.page_count;
  const bool closed = tavern::close_database(*database).has_value();
  std::filesystem::remove(file);
  if (!closed || measurements.reads.empty() || measurements.mutations.empty())
    return false;
  std::cout << "mixed_workload=" << name(workload) << " seed=" << seed
            << " mixed_seed="
            << (seed ^ 0x4D382E31776F726BULL ^
                static_cast<std::uint64_t>(workload))
            << " operations=" << mixed_operations
            << " warmup_operations=" << mixed_warmup_operations
            << " warmup_total_us=" << warmup_us
            << " measured_total_us=" << measured_us
            << " operation_time_us=" << measurements.operation_us
            << " operations_per_second="
            << mixed_operations * 1000000.0 / measurements.operation_us
            << " reads=" << measurements.reads.size() << " mutations_attempted="
            << (mixed_operations - measurements.reads.size())
            << " read_follow_membership=" << measurements.read_kinds[0]
            << " read_friend_membership=" << measurements.read_kinds[1]
            << " read_enumeration=" << measurements.read_kinds[2]
            << " read_mutual=" << measurements.read_kinds[3]
            << " mutation_follow=" << measurements.mutation_kinds[0]
            << " mutation_unfollow=" << measurements.mutation_kinds[1]
            << " mutation_befriend=" << measurements.mutation_kinds[2]
            << " mutation_unfriend=" << measurements.mutation_kinds[3]
            << " commits=" << measurements.commits
            << " noops=" << measurements.noops
            << " rejected=" << measurements.rejected
            << " full_reachability_scans=" << measurements.scans
            << " reuse_commits=" << measurements.reuse_commits
            << " append_commits=" << measurements.append_commits
            << " appended_pages=" << measurements.appended_pages
            << " initial_pages=" << initial_pages
            << " final_pages=" << final_pages
            << " result_checksum=" << measurements.result_checksum;
  print_stats("read", measurements.reads);
  print_stats("mutation", measurements.mutations);
  print_stats("warm_mutation", measurements.warm_mutations);
  print_stats("cold_mutation", measurements.cold_mutations);
  std::cout << '\n';
  return true;
}

struct ScaleFixture {
  std::size_t relationships;
  std::size_t population;
  std::size_t high_degree_vertices;
};

auto scaled_vertex(std::uint64_t& state, const ScaleFixture& fixture)
    -> std::size_t {
  if (next(state) % 100U < 60U)
    return static_cast<std::size_t>(next(state) % fixture.high_degree_vertices);
  return static_cast<std::size_t>(next(state) % fixture.population);
}

auto add_scaled_relationships(Model& model, const ScaleFixture& fixture,
                              std::uint64_t& state) -> bool {
  const std::size_t follow_count = fixture.relationships / 2U;
  std::size_t follows = 0;
  std::size_t friendships = 0;
  while (follows < follow_count) {
    const std::size_t left = scaled_vertex(state, fixture);
    const std::size_t right =
        static_cast<std::size_t>(next(state) % fixture.population);
    if (left == right || model.follow(left, right)) continue;
    model.follow(left, right) = true;
    ++follows;
  }
  while (friendships < fixture.relationships - follow_count) {
    const std::size_t left = scaled_vertex(state, fixture);
    const std::size_t right =
        static_cast<std::size_t>(next(state) % fixture.population);
    if (left == right || model.friend_of(left, right)) continue;
    model.friend_of(left, right) = true;
    model.friend_of(right, left) = true;
    ++friendships;
  }
  return true;
}

auto build_scaled_fixture(tavern::Database& database, Model& model,
                          const ScaleFixture& fixture) -> bool {
  std::uint64_t state = seed ^ 0x4D382E322D666978ULL ^ fixture.relationships;
  if (!add_scaled_relationships(model, fixture, state)) return false;
  tavern::PageId root = database_impl(database).commit.root;
  for (std::size_t left = 0; left < model.population; ++left) {
    std::vector<tavern::PersonId> following;
    std::vector<tavern::PersonId> followers;
    std::vector<tavern::PersonId> friends;
    for (std::size_t right = 0; right < model.population; ++right) {
      if (model.follow(left, right))
        following.push_back(static_cast<tavern::PersonId>(right + 1U));
      if (model.follow(right, left))
        followers.push_back(static_cast<tavern::PersonId>(right + 1U));
      if (model.friend_of(left, right))
        friends.push_back(static_cast<tavern::PersonId>(right + 1U));
    }
    const auto following_tree =
        tavern::build_adjacency_tree(database_impl(database).store, following);
    const auto followers_tree =
        tavern::build_adjacency_tree(database_impl(database).store, followers);
    const auto friends_tree =
        tavern::build_adjacency_tree(database_impl(database).store, friends);
    if (!following_tree || !followers_tree || !friends_tree) return false;
    tavern::PatronRecord record{};
    record.id = static_cast<tavern::PersonId>(left + 1U);
    record.following = *following_tree;
    record.followers = *followers_tree;
    record.friends = *friends_tree;
    const auto next_root =
        tavern::directory_insert(database_impl(database).store, root, record);
    if (!next_root) return false;
    root = *next_root;
  }
  return tavern::commit_root(database_impl(database).store,
                             database_impl(database).commit, root)
      .has_value();
}

auto run_scaled_mixed(const ScaleFixture& fixture, MixedWorkload workload)
    -> bool {
  const auto file =
      std::filesystem::temp_directory_path() /
      ("tavern-mixed-scale-" + std::to_string(fixture.relationships) + "-" +
       std::string(name(workload)) + "-" +
       std::to_string(static_cast<long long>(getpid())) + ".db");
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  Model model(fixture.population);
  const auto build_start = Clock::now();
  if (!database || !build_scaled_fixture(*database, model, fixture))
    return false;
  const double fixture_build_us =
      std::chrono::duration<double, std::micro>(Clock::now() - build_start)
          .count();
  const tavern::PageId initial_pages =
      database_impl(*database).store.page_count;
  std::uint64_t state = seed ^ 0x4D382E322D6D6978ULL ^ fixture.relationships ^
                        static_cast<std::uint64_t>(workload);
  std::uint64_t checkpoint_state = state ^ 0x9E3779B97F4A7C15ULL;
  MixedMeasurements measurements;
  const auto run_operation = [&](bool measure) -> bool {
    const bool is_read = next(state) % 100U < read_percent(workload);
    const std::size_t kind = static_cast<std::size_t>(next(state) % 4U);
    const std::size_t left = scaled_vertex(state, fixture);
    const std::size_t right =
        static_cast<std::size_t>(next(state) % fixture.population);
    if (is_read) {
      const auto start = Clock::now();
      const bool valid = execute_mixed_read(*database, model, kind, left, right,
                                            measurements.result_checksum);
      const double elapsed =
          std::chrono::duration<double, std::micro>(Clock::now() - start)
              .count();
      if (measure) {
        ++measurements.read_kinds[kind];
        measurements.reads.push_back(elapsed);
        if (kind < 2U) measurements.membership_reads.push_back(elapsed);
        if (kind == 3U) measurements.mutual_reads.push_back(elapsed);
        measurements.operation_us += elapsed;
      }
      return valid;
    }
    if (measure) ++measurements.mutation_kinds[kind];
    return execute_mixed_mutation(*database, model, kind, left, right,
                                  measurements, measure);
  };
  const auto warmup_start = Clock::now();
  for (std::size_t index = 0; index < mixed_warmup_operations; ++index)
    if (!run_operation(false)) return false;
  const double warmup_us =
      std::chrono::duration<double, std::micro>(Clock::now() - warmup_start)
          .count();
  if (!matches_checkpoint(*database, model, checkpoint_state)) return false;
  tavern::reset_free_page_pool_scan_count_for_test();
  for (std::size_t index = 0; index < mixed_operations; ++index) {
    if (!run_operation(true)) return false;
    if ((index + 1U) % 500U == 0U &&
        !matches_checkpoint(*database, model, checkpoint_state)) {
      return false;
    }
  }
  measurements.scans = tavern::free_page_pool_scan_count_for_test();
  Samples scan_costs;
  tavern::ReachabilityScanDiagnostics diagnostics;
  for (std::size_t index = 0; index < 5; ++index) {
    const auto scan =
        tavern::profile_reachable_pages(database_impl(*database).store);
    if (!scan) return false;
    diagnostics = scan->diagnostics;
    scan_costs.push_back(microseconds(diagnostics.total));
  }
  const tavern::PageId final_pages = database_impl(*database).store.page_count;
  const bool closed = tavern::close_database(*database).has_value();
  std::filesystem::remove(file);
  if (!closed || measurements.reads.empty() || measurements.mutations.empty())
    return false;
  std::cout << "scaled_mixed_workload=" << name(workload)
            << " relationships=" << fixture.relationships
            << " population=" << fixture.population
            << " high_degree_vertices=" << fixture.high_degree_vertices
            << " seed=" << seed << " mixed_seed="
            << (seed ^ 0x4D382E322D6D6978ULL ^ fixture.relationships ^
                static_cast<std::uint64_t>(workload))
            << " fixture_build_us=" << fixture_build_us
            << " operations=" << mixed_operations
            << " warmup_operations=" << mixed_warmup_operations
            << " warmup_total_us=" << warmup_us << " operations_per_second="
            << mixed_operations * 1000000.0 / measurements.operation_us
            << " reads=" << measurements.reads.size() << " mutations_attempted="
            << (mixed_operations - measurements.reads.size())
            << " commits=" << measurements.commits
            << " noops=" << measurements.noops
            << " rejected=" << measurements.rejected
            << " full_reachability_scans=" << measurements.scans
            << " reuse_commits=" << measurements.reuse_commits
            << " append_commits=" << measurements.append_commits
            << " appended_pages=" << measurements.appended_pages
            << " initial_physical_data_pages="
            << initial_pages - tavern::first_data_page
            << " physical_data_pages=" << final_pages - tavern::first_data_page
            << " reachable_data_pages=" << diagnostics.reachable_data_pages
            << " read_follow_membership=" << measurements.read_kinds[0]
            << " read_friend_membership=" << measurements.read_kinds[1]
            << " read_enumeration=" << measurements.read_kinds[2]
            << " read_mutual=" << measurements.read_kinds[3]
            << " mutation_follow=" << measurements.mutation_kinds[0]
            << " mutation_unfollow=" << measurements.mutation_kinds[1]
            << " mutation_befriend=" << measurements.mutation_kinds[2]
            << " mutation_unfriend=" << measurements.mutation_kinds[3]
            << " result_checksum=" << measurements.result_checksum;
  print_stats("read", measurements.reads);
  print_stats("membership", measurements.membership_reads);
  print_stats("mutual", measurements.mutual_reads);
  print_stats("mutation", measurements.mutations);
  print_stats("warm_mutation", measurements.warm_mutations);
  print_stats("cold_mutation", measurements.cold_mutations);
  print_stats("reachability_scan", scan_costs);
  std::cout << '\n';
  return true;
}

auto profile_liveness_shadow(const ScaleFixture& fixture) -> bool {
  const auto file =
      std::filesystem::temp_directory_path() /
      ("tavern-reclamation-shadow-" + std::to_string(fixture.relationships) +
       "-" + std::to_string(static_cast<long long>(getpid())) + ".db");
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  Model model(fixture.population);
  if (!database || !build_scaled_fixture(*database, model, fixture) ||
      !tavern::initialize_page_liveness_shadow_for_test(*database)) {
    return false;
  }
  Samples updates;
  Samples activated;
  Samples deactivated;
  Samples free_candidates;
  for (std::size_t index = 0; index < 100; ++index) {
    const tavern::PersonId a =
        static_cast<tavern::PersonId>(index % fixture.population + 1U);
    tavern::PersonId b = static_cast<tavern::PersonId>(
        (index * 29U + fixture.population / 2U) % fixture.population + 1U);
    if (a == b) b = b % fixture.population + 1U;
    const auto generation = database_impl(*database).commit.generation;
    if (!tavern::follow_with_index_reuse_for_test(*database, a, b))
      return false;
    if (database_impl(*database).commit.generation == generation) continue;
    if (!database_impl(*database).page_liveness_shadow) return false;
    const auto& diagnostics =
        database_impl(*database).page_liveness_shadow_diagnostics;
    updates.push_back(microseconds(diagnostics.elapsed));
    activated.push_back(static_cast<double>(diagnostics.activated_pages));
    deactivated.push_back(static_cast<double>(diagnostics.deactivated_pages));
    free_candidates.push_back(static_cast<double>(diagnostics.free_candidates));
  }
  Samples scans;
  for (std::size_t index = 0; index < 5; ++index) {
    const auto scan =
        tavern::profile_reachable_pages(database_impl(*database).store);
    if (!scan) return false;
    scans.push_back(microseconds(scan->diagnostics.total));
  }
  const bool closed = tavern::close_database(*database).has_value();
  std::filesystem::remove(file);
  if (!closed || updates.empty()) return false;
  std::cout << "shadow_relationships=" << fixture.relationships
            << " shadow_updates=" << updates.size();
  print_stats("incremental_update", updates);
  print_distribution("activated_pages", activated);
  print_distribution("deactivated_pages", deactivated);
  print_distribution("free_candidates", free_candidates);
  print_stats("full_scan", scans);
  std::cout << '\n';
  return true;
}

auto profile_incremental_follow(const ScaleFixture& fixture) -> bool {
  const auto run = [&](bool incremental) -> bool {
    const auto file =
        std::filesystem::temp_directory_path() /
        ("tavern-reclamation-" + std::to_string(fixture.relationships) +
         (incremental ? "-index.db" : "-pool.db"));
    std::filesystem::remove(file);
    auto database = tavern::create_database(file);
    Model model(fixture.population);
    if (!database || !build_scaled_fixture(*database, model, fixture))
      return false;
    const tavern::PageId initial_pages =
        database_impl(*database).store.page_count;
    double reconstruction_us = 0;
    if (incremental) {
      const auto start = Clock::now();
      if (!tavern::initialize_page_liveness_shadow_for_test(*database))
        return false;
      reconstruction_us =
          std::chrono::duration<double, std::micro>(Clock::now() - start)
              .count();
    }
    tavern::reset_free_page_pool_scan_count_for_test();
    tavern::reset_reachability_scan_count_for_test();
    Samples latency;
    Samples updates;
    std::size_t reused = 0;
    std::size_t appended = 0;
    for (std::size_t index = 0; index < 100; ++index) {
      const std::size_t left = index % fixture.population;
      std::size_t right =
          (index * 29U + fixture.population / 2U) % fixture.population;
      while (left == right || model.follow(left, right))
        right = (right + 1U) % fixture.population;
      const auto start = Clock::now();
      const auto result =
          incremental ? tavern::follow_with_index_reuse_for_test(
                            *database, static_cast<tavern::PersonId>(left + 1U),
                            static_cast<tavern::PersonId>(right + 1U))
                      : tavern::follow(
                            *database, static_cast<tavern::PersonId>(left + 1U),
                            static_cast<tavern::PersonId>(right + 1U));
      if (!result) return false;
      model.follow(left, right) = true;
      latency.push_back(
          std::chrono::duration<double, std::micro>(Clock::now() - start)
              .count());
      const auto& diagnostics =
          database_impl(*database).page_liveness_shadow_diagnostics;
      reused += diagnostics.reused_pages;
      appended += diagnostics.appended_pages;
      if (incremental) {
        updates.push_back(microseconds(diagnostics.elapsed));
      }
    }
    const std::size_t pool_scans = tavern::free_page_pool_scan_count_for_test();
    const std::size_t reachability_scans =
        tavern::reachability_scan_count_for_test();
    const tavern::PageId final_pages =
        database_impl(*database).store.page_count;
    const bool closed = tavern::close_database(*database).has_value();
    std::filesystem::remove(file);
    if (!closed) return false;
    std::cout << "follow_mode=" << (incremental ? "index" : "pool")
              << " relationships=" << fixture.relationships
              << " initial_reconstruction_us=" << reconstruction_us
              << " full_pool_scans=" << pool_scans
              << " full_reachability_scans=" << reachability_scans
              << " reused_pages=" << reused << " appended_pages=" << appended
              << " physical_growth=" << final_pages - initial_pages;
    print_stats("follow", latency);
    if (incremental) print_stats("incremental_update", updates);
    std::cout << '\n';
    return true;
  };
  return run(false) && run(true);
}

}  // namespace

auto main() -> int {
  std::cout << std::fixed << std::setprecision(2) << "compiler=" << __VERSION__
            << " seed=" << seed << " operations_per_workload=" << operations
            << " unit=us\n";
  for (const Workload workload :
       {Workload::sparse, Workload::dense, Workload::insertion_heavy,
        Workload::churn_heavy}) {
    if (!run(workload)) return 1;
  }
  constexpr std::array<std::pair<const char*, FixtureBuilder>, 4> fixtures{{
      {"large-sparse", build_large_sparse_fixture},
      {"small-dense", build_small_dense_fixture},
      {"large-unreachable", build_large_unreachable_fixture},
      {"cow-sharing", build_cow_sharing_fixture},
  }};
  for (const auto& [fixture, builder] : fixtures) {
    if (!profile_fixture(fixture, builder)) return 1;
  }
  for (const std::size_t degree : {10U, 100U, 1000U, 10000U}) {
    if (!profile_degree(degree)) return 1;
  }
  for (const MixedWorkload workload :
       {MixedWorkload::read_heavy, MixedWorkload::balanced,
        MixedWorkload::write_heavy}) {
    if (!run_mixed(workload)) return 1;
  }
  for (const ScaleFixture fixture :
       {ScaleFixture{1000, 128, 8}, ScaleFixture{10000, 512, 8},
        ScaleFixture{100000, 2048, 8}}) {
    for (const MixedWorkload workload :
         {MixedWorkload::read_heavy, MixedWorkload::balanced,
          MixedWorkload::write_heavy}) {
      if (!run_scaled_mixed(fixture, workload)) return 1;
    }
  }
  for (const ScaleFixture fixture :
       {ScaleFixture{1000, 128, 8}, ScaleFixture{100000, 2048, 8}}) {
    if (!profile_liveness_shadow(fixture)) return 1;
  }
  for (const ScaleFixture fixture :
       {ScaleFixture{1000, 128, 8}, ScaleFixture{10000, 512, 8},
        ScaleFixture{100000, 2048, 8}}) {
    if (!profile_incremental_follow(fixture)) return 1;
  }
  return 0;
}
