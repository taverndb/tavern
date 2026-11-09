#include "page_allocation_session.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <filesystem>
#include <unordered_set>

#include "page_store_internal.hpp"
#include "persistent_relationship.hpp"
#include "protected_roots.hpp"

namespace {

auto path(const char* suffix) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         (std::string("tavern-allocation-session-") +
          std::to_string(static_cast<long long>(getpid())) + suffix);
}

auto read_raw(const tavern::PageStore& store, tavern::PageId page,
              std::array<std::byte, tavern::page_size>& bytes) -> bool {
  return pread(store.file_descriptor, bytes.data(), bytes.size(),
               static_cast<off_t>(page * tavern::page_size)) ==
         static_cast<ssize_t>(bytes.size());
}

auto metadata_equal(const tavern::CommitMetadata& left,
                    const tavern::CommitMetadata& right) -> bool {
  return left.generation == right.generation && left.root == right.root &&
         left.committed_page_count == right.committed_page_count;
}

auto encoded_data_page()
    -> std::expected<std::array<std::byte, tavern::page_size>, tavern::Error> {
  const auto file = path("-encoded.db");
  std::filesystem::remove(file);
  auto store = tavern::create_page_store(file);
  const std::array<std::byte, 1> payload{std::byte{0x5A}};
  const auto page =
      store ? tavern::append_page(*store, tavern::PageType::data, payload)
            : std::expected<tavern::PageId, tavern::Error>(
                  std::unexpected(store.error()));
  std::array<std::byte, tavern::page_size> bytes{};
  const bool read = store && page && read_raw(*store, *page, bytes);
  if (store) (void)tavern::close_page_store(*store);
  std::filesystem::remove(file);
  return read ? std::expected<std::array<std::byte, tavern::page_size>,
                              tavern::Error>(bytes)
              : std::expected<std::array<std::byte, tavern::page_size>,
                              tavern::Error>(
                    std::unexpected(tavern::Error::io_failure));
}

auto empty_pool_appends(const std::array<std::byte, tavern::page_size>& encoded)
    -> bool {
  const auto file = path("-empty.db");
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  if (!database) return false;
  auto pool = tavern::build_free_page_pool(*database);
  const tavern::PageId before = database_impl(*database).store.page_count;
  bool result = false;
  {
    auto session =
        pool ? tavern::storage_detail::begin_page_allocation_session(
                   *database, std::move(*pool))
             : std::expected<tavern::storage_detail::PageAllocationSession,
                             tavern::Error>(std::unexpected(pool.error()));
    const auto page = session ? tavern::storage_detail::allocate_encoded_page(
                                    *session, encoded)
                              : std::expected<tavern::PageId, tavern::Error>(
                                    std::unexpected(session.error()));
    const auto& journal =
        session ? tavern::storage_detail::allocation_journal(*session)
                : std::vector<tavern::storage_detail::AllocationJournalEntry>{};
    result = page && *page == before && session && session->append_phase &&
             database_impl(*database).store.page_count == before + 1 &&
             journal.size() == 1 && journal.front().page == *page &&
             journal.front().source ==
                 tavern::storage_detail::AllocationSource::appended;
  }
  const auto closed = tavern::close_database(*database);
  std::filesystem::remove(file);
  return result && closed.has_value();
}

auto reuse_then_append(const std::array<std::byte, tavern::page_size>& encoded)
    -> bool {
  const auto file = path("-reuse.db");
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  const std::array<std::byte, 1> payload{std::byte{1}};
  if (!database || !tavern::follow(*database, 1, 2) ||
      !tavern::befriend(*database, 3, 4) ||
      !tavern::append_page(database_impl(*database).store,
                           tavern::PageType::data, payload))
    return false;
  const auto metadata_a = tavern::read_metadata(database_impl(*database).store,
                                                tavern::metadata_slot_a);
  const auto metadata_b = tavern::read_metadata(database_impl(*database).store,
                                                tavern::metadata_slot_b);
  auto pool = tavern::build_free_page_pool(*database);
  if (!metadata_a || !metadata_b ||
      metadata_a->root == tavern::invalid_page_id ||
      metadata_b->root == tavern::invalid_page_id ||
      metadata_a->root == metadata_b->root || !pool || pool->candidates.empty())
    return false;
  const auto expected = pool->candidates;
  bool below_previous_extent = false;
  for (const tavern::PageId page : expected)
    below_previous_extent =
        below_previous_extent || page < database_impl(*database).commit.extent;
  if (!below_previous_extent) return false;
  const tavern::PageId before = database_impl(*database).store.page_count;
  bool result = true;
  std::vector<tavern::storage_detail::AllocationJournalEntry> journal;
  {
    auto session = tavern::storage_detail::begin_page_allocation_session(
        *database, std::move(*pool));
    if (!session || tavern::close_database(*database)) return false;
    for (const tavern::PageId expected_page : expected) {
      const auto page =
          tavern::storage_detail::allocate_encoded_page(*session, encoded);
      result = result && page && *page == expected_page &&
               database_impl(*database).store.page_count == before &&
               !session->append_phase;
    }
    const auto appended =
        tavern::storage_detail::allocate_encoded_page(*session, encoded);
    const auto appended_again =
        tavern::storage_detail::allocate_encoded_page(*session, encoded);
    result = result && appended && appended_again && *appended == before &&
             *appended_again == before + 1 && session->append_phase &&
             database_impl(*database).store.page_count == before + 2 &&
             tavern::persistent_is_following(
                 database_impl(*database).store,
                 database_impl(*database).commit.root,
                 database_impl(*database).commit.extent, 1, 2)
                 .value_or(false);
    journal = tavern::storage_detail::take_allocation_journal(*session);
    result =
        result && tavern::storage_detail::allocation_journal(*session).empty();
  }
  const auto after_a = tavern::read_metadata(database_impl(*database).store,
                                             tavern::metadata_slot_a);
  const auto after_b = tavern::read_metadata(database_impl(*database).store,
                                             tavern::metadata_slot_b);
  const auto protected_roots =
      tavern::scan_protected_directories(database_impl(*database).store);
  const auto generation = database_impl(*database).commit.generation;
  if (!after_a || !after_b || !protected_roots ||
      protected_roots->roots.size() != 2 ||
      !metadata_equal(*metadata_a, *after_a) ||
      !metadata_equal(*metadata_b, *after_b) ||
      !tavern::close_database(*database))
    return false;
  auto reopened = tavern::open_database(file);
  const bool recovered =
      reopened && database_impl(*reopened).commit.generation == generation &&
      tavern::is_following(*reopened, 1, 2).value_or(false);
  if (reopened) (void)tavern::close_database(*reopened);
  std::filesystem::remove(file);
  if (journal.size() != expected.size() + 2U) return false;
  for (std::size_t index = 0; index < expected.size(); ++index)
    if (journal[index].page != expected[index] ||
        journal[index].source !=
            tavern::storage_detail::AllocationSource::reused)
      return false;
  return result && journal[expected.size()].page == before &&
         journal[expected.size()].source ==
             tavern::storage_detail::AllocationSource::appended &&
         journal[expected.size() + 1U].page == before + 1U &&
         journal[expected.size() + 1U].source ==
             tavern::storage_detail::AllocationSource::appended &&
         recovered;
}

auto failures_and_rejections(
    const std::array<std::byte, tavern::page_size>& encoded) -> bool {
  const auto file = path("-failure.db");
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  if (!database || !tavern::follow(*database, 1, 2)) return false;
  const auto metadata_a = tavern::read_metadata(database_impl(*database).store,
                                                tavern::metadata_slot_a);
  const auto metadata_b = tavern::read_metadata(database_impl(*database).store,
                                                tavern::metadata_slot_b);
  auto first_pool = tavern::build_free_page_pool(*database);
  auto second_pool = tavern::build_free_page_pool(*database);
  if (!first_pool || !second_pool) return false;
  {
    auto session = tavern::storage_detail::begin_page_allocation_session(
        *database, std::move(*first_pool));
    if (!session || tavern::storage_detail::begin_page_allocation_session(
                        *database, std::move(*second_pool)))
      return false;
    database_impl(*database).store.fail_reusable_write_after_for_test = 1;
    if (tavern::storage_detail::allocate_encoded_page(*session, encoded) ||
        tavern::storage_detail::allocate_encoded_page(*session, encoded) ||
        !session->failed || session->reserved_pages.size() != 1 ||
        !tavern::storage_detail::allocation_journal(*session).empty())
      return false;
  }
  const auto after_a = tavern::read_metadata(database_impl(*database).store,
                                             tavern::metadata_slot_a);
  const auto after_b = tavern::read_metadata(database_impl(*database).store,
                                             tavern::metadata_slot_b);
  if (!metadata_a || !metadata_b || !after_a || !after_b ||
      !metadata_equal(*metadata_a, *after_a) ||
      !metadata_equal(*metadata_b, *after_b))
    return false;
  auto pool = tavern::build_free_page_pool(*database);
  const auto other = tavern::open_database(file);
  if (!pool || other || other.error() != tavern::Error::database_locked)
    return false;
  auto closed_pool = tavern::build_free_page_pool(*database);
  if (!closed_pool || !tavern::close_database(*database)) return false;
  const auto closed = tavern::storage_detail::begin_page_allocation_session(
      *database, std::move(*closed_pool));
  std::filesystem::remove(file);
  return !closed;
}

auto failed_append_is_not_journaled(
    const std::array<std::byte, tavern::page_size>& encoded) -> bool {
  const auto file = path("-append-failure.db");
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  auto pool = database ? tavern::build_free_page_pool(*database)
                       : std::expected<tavern::FreePagePool, tavern::Error>(
                             std::unexpected(tavern::Error::io_failure));
  if (!database || !pool) return false;
  const tavern::PageId before = database_impl(*database).store.page_count;
  database_impl(*database).store.fail_append_after_for_test = 0;
  bool result = false;
  {
    auto session = tavern::storage_detail::begin_page_allocation_session(
        *database, std::move(*pool));
    const auto failed = session ? tavern::storage_detail::allocate_encoded_page(
                                      *session, encoded)
                                : std::expected<tavern::PageId, tavern::Error>(
                                      std::unexpected(session.error()));
    const auto repeated =
        session
            ? tavern::storage_detail::allocate_encoded_page(*session, encoded)
            : std::expected<tavern::PageId, tavern::Error>(
                  std::unexpected(session.error()));
    result = session && !failed && !repeated && session->failed &&
             tavern::storage_detail::allocation_journal(*session).empty() &&
             database_impl(*database).store.page_count == before;
  }
  const auto closed = tavern::close_database(*database);
  std::filesystem::remove(file);
  return result && closed.has_value();
}

auto intermediate_cow_pages_are_journaled() -> bool {
  const auto file = path("-intermediate.db");
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  if (!database || !tavern::follow(*database, 1, 2)) return false;
  const auto before_a = tavern::read_metadata(database_impl(*database).store,
                                              tavern::metadata_slot_a);
  const auto before_b = tavern::read_metadata(database_impl(*database).store,
                                              tavern::metadata_slot_b);
  auto pool = tavern::build_free_page_pool(*database);
  if (!before_a || !before_b || !pool) return false;
  std::vector<tavern::storage_detail::AllocationJournalEntry> journal;
  std::size_t final_pages = 0;
  bool built = false;
  {
    auto session = tavern::storage_detail::begin_page_allocation_session(
        *database, std::move(*pool));
    const auto candidate =
        session ? tavern::persistent_follow(
                      database_impl(*database).store,
                      database_impl(*database).commit.root,
                      database_impl(*database).commit.extent, 1, 3)
                : std::expected<tavern::PageId, tavern::Error>(
                      std::unexpected(session.error()));
    std::unordered_set<tavern::PageId> candidate_pages;
    if (!session || !candidate ||
        !tavern::traverse_directory(
            database_impl(*database).store, *candidate,
            database_impl(*database).store.page_count,
            [&](tavern::PageId page,
                tavern::PageType) -> std::expected<void, tavern::Error> {
              candidate_pages.insert(page);
              return std::expected<void, tavern::Error>{};
            },
            [&](const tavern::PatronRecord& record)
                -> std::expected<void, tavern::Error> {
              for (const tavern::AdjacencyDescriptor& descriptor :
                   {record.following, record.followers, record.friends}) {
                const auto traversed = tavern::traverse_adjacency_pages(
                    database_impl(*database).store, descriptor,
                    database_impl(*database).store.page_count,
                    [&](tavern::PageId page, tavern::PageType)
                        -> std::expected<void, tavern::Error> {
                      candidate_pages.insert(page);
                      return std::expected<void, tavern::Error>{};
                    });
                if (!traversed) return traversed;
              }
              return std::expected<void, tavern::Error>{};
            })) {
      return false;
    }
    journal = tavern::storage_detail::take_allocation_journal(*session);
    for (const auto& entry : journal)
      final_pages += candidate_pages.contains(entry.page);
    built = true;
  }
  const auto after_a = tavern::read_metadata(database_impl(*database).store,
                                             tavern::metadata_slot_a);
  const auto after_b = tavern::read_metadata(database_impl(*database).store,
                                             tavern::metadata_slot_b);
  const bool result = built && !journal.empty() &&
                      final_pages < journal.size() && after_a && after_b &&
                      metadata_equal(*before_a, *after_a) &&
                      metadata_equal(*before_b, *after_b);
  const auto closed = tavern::close_database(*database);
  std::filesystem::remove(file);
  return result && closed.has_value();
}

}  // namespace

auto main() -> int {
  const auto encoded = encoded_data_page();
  return encoded && empty_pool_appends(*encoded) &&
                 reuse_then_append(*encoded) &&
                 failures_and_rejections(*encoded) &&
                 failed_append_is_not_journaled(*encoded) &&
                 intermediate_cow_pages_are_journaled()
             ? 0
             : 1;
}
