#include <unistd.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <unordered_set>

#include "page_allocation_session.hpp"
#include "page_store_internal.hpp"
#include "reachability.hpp"

namespace {

auto path(const char* suffix) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         (std::string("tavern-index-session-") +
          std::to_string(static_cast<long long>(getpid())) + suffix);
}

auto encoded()
    -> std::expected<std::array<std::byte, tavern::page_size>, tavern::Error> {
  const auto file = path("-encoded.db");
  std::filesystem::remove(file);
  auto store = tavern::create_page_store(file);
  const std::array<std::byte, 1> payload{std::byte{0x5A}};
  const auto page =
      store ? tavern::append_page(*store, tavern::PageType::data, payload)
            : std::expected<tavern::PageId, tavern::Error>(
                  std::unexpected(tavern::Error::io_failure));
  std::array<std::byte, tavern::page_size> bytes{};
  const bool read = store && page &&
                    pread(store->file_descriptor, bytes.data(), bytes.size(),
                          static_cast<off_t>(*page * tavern::page_size)) ==
                        static_cast<ssize_t>(bytes.size());
  if (store) (void)tavern::close_page_store(*store);
  std::filesystem::remove(file);
  return read ? std::expected<std::array<std::byte, tavern::page_size>,
                              tavern::Error>(bytes)
              : std::expected<std::array<std::byte, tavern::page_size>,
                              tavern::Error>(
                    std::unexpected(tavern::Error::io_failure));
}

auto metadata_equal(const tavern::CommitMetadata& left,
                    const tavern::CommitMetadata& right) -> bool {
  return left.generation == right.generation && left.root == right.root &&
         left.committed_page_count == right.committed_page_count;
}

auto safe(const tavern::Database& database,
          const std::vector<tavern::PageId>& candidates) -> bool {
  const auto scan = tavern::scan_reachable_pages(database_impl(database).store);
  if (!scan) return false;
  std::unordered_set<tavern::PageId> unique;
  for (const tavern::PageId page : candidates)
    if (page < tavern::first_data_page ||
        page >= database_impl(database).store.page_count ||
        scan->reachable[static_cast<std::size_t>(page)] ||
        !unique.insert(page).second)
      return false;
  return true;
}

auto reuse_order_and_append(
    const std::array<std::byte, tavern::page_size>& bytes) -> bool {
  const auto file = path("-reuse.db");
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  if (!database || !tavern::follow(*database, 1, 2) ||
      !tavern::befriend(*database, 3, 4) ||
      !tavern::initialize_page_liveness_shadow_for_test(*database)) {
    return false;
  }
  const auto before_a = tavern::read_metadata(database_impl(*database).store,
                                              tavern::metadata_slot_a);
  const auto before_b = tavern::read_metadata(database_impl(*database).store,
                                              tavern::metadata_slot_b);
  const auto candidates =
      tavern::page_liveness_shadow_candidates_for_test(*database);
  if (!before_a || !before_b || !candidates || candidates->empty() ||
      !std::is_sorted(candidates->begin(), candidates->end()) ||
      !safe(*database, *candidates)) {
    return false;
  }
  const tavern::PageId append_start = database_impl(*database).store.page_count;
  std::vector<tavern::storage_detail::AllocationJournalEntry> journal;
  {
    auto session =
        tavern::storage_detail::begin_index_backed_page_allocation_session(
            *database, *database_impl(*database).page_liveness_shadow);
    if (!session || !session->index_backed) return false;
    for (const tavern::PageId expected : *candidates) {
      const auto page =
          tavern::storage_detail::allocate_encoded_page(*session, bytes);
      if (!page || *page != expected) return false;
    }
    const auto appended =
        tavern::storage_detail::allocate_encoded_page(*session, bytes);
    if (!appended || *appended != append_start) return false;
    journal = tavern::storage_detail::take_allocation_journal(*session);
  }
  const auto after_a = tavern::read_metadata(database_impl(*database).store,
                                             tavern::metadata_slot_a);
  const auto after_b = tavern::read_metadata(database_impl(*database).store,
                                             tavern::metadata_slot_b);
  bool ordered = journal.size() == candidates->size() + 1U;
  for (std::size_t index = 0; ordered && index < candidates->size(); ++index)
    ordered = journal[index].page == (*candidates)[index] &&
              journal[index].source ==
                  tavern::storage_detail::AllocationSource::reused;
  ordered = ordered && journal.back().page == append_start &&
            journal.back().source ==
                tavern::storage_detail::AllocationSource::appended;
  const bool closed = tavern::close_database(*database).has_value();
  auto reopened = tavern::open_database(file);
  const bool recovered =
      reopened && tavern::is_following(*reopened, 1, 2).value_or(false) &&
      tavern::are_friends(*reopened, 3, 4).value_or(false) &&
      tavern::close_database(*reopened);
  std::filesystem::remove(file);
  return ordered && after_a && after_b && metadata_equal(*before_a, *after_a) &&
         metadata_equal(*before_b, *after_b) && closed && recovered;
}

auto append_and_rejection_cases(
    const std::array<std::byte, tavern::page_size>& bytes) -> bool {
  const auto file = path("-append.db");
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  if (!database || !tavern::initialize_page_liveness_shadow_for_test(*database))
    return false;
  const tavern::PageId before = database_impl(*database).store.page_count;
  {
    auto session =
        tavern::storage_detail::begin_index_backed_page_allocation_session(
            *database, *database_impl(*database).page_liveness_shadow);
    const auto page =
        session ? tavern::storage_detail::allocate_encoded_page(*session, bytes)
                : std::expected<tavern::PageId, tavern::Error>(
                      std::unexpected(session.error()));
    if (!session || !page || *page != before ||
        tavern::page_liveness_shadow_candidates_for_test(*database)) {
      return false;
    }
  }
  if (!tavern::close_database(*database)) return false;
  std::filesystem::remove(file);
  database = tavern::create_database(file);
  if (!database || !tavern::initialize_page_liveness_shadow_for_test(*database))
    return false;
  database_impl(*database).store.fail_append_after_for_test = 0;
  {
    auto session =
        tavern::storage_detail::begin_index_backed_page_allocation_session(
            *database, *database_impl(*database).page_liveness_shadow);
    if (!session ||
        tavern::storage_detail::allocate_encoded_page(*session, bytes) ||
        !session->failed || database_impl(*database).page_liveness_shadow ||
        !tavern::storage_detail::allocation_journal(*session).empty()) {
      return false;
    }
  }
  const bool closed = tavern::close_database(*database).has_value();
  std::filesystem::remove(file);
  return closed;
}

auto stale_invalid_and_partial_rejection(
    const std::array<std::byte, tavern::page_size>& bytes) -> bool {
  const auto file = path("-failure.db");
  std::filesystem::remove(file);
  auto database = tavern::create_database(file);
  if (!database || !tavern::follow(*database, 1, 2) ||
      !tavern::befriend(*database, 3, 4) ||
      !tavern::initialize_page_liveness_shadow_for_test(*database)) {
    return false;
  }
  const std::array<std::byte, 1> payload{std::byte{0x11}};
  if (!tavern::append_page(database_impl(*database).store,
                           tavern::PageType::data, payload) ||
      tavern::storage_detail::begin_index_backed_page_allocation_session(
          *database, *database_impl(*database).page_liveness_shadow)) {
    return false;
  }
  if (!tavern::initialize_page_liveness_shadow_for_test(*database))
    return false;
  database_impl(*database).page_liveness_shadow->valid = false;
  if (tavern::storage_detail::begin_index_backed_page_allocation_session(
          *database, *database_impl(*database).page_liveness_shadow)) {
    return false;
  }
  database_impl(*database).page_liveness_shadow->valid = true;
  database_impl(*database).store.fail_reusable_write_after_for_test = 1;
  {
    auto session =
        tavern::storage_detail::begin_index_backed_page_allocation_session(
            *database, *database_impl(*database).page_liveness_shadow);
    if (!session ||
        tavern::storage_detail::allocate_encoded_page(*session, bytes) ||
        !session->failed || database_impl(*database).page_liveness_shadow ||
        !tavern::storage_detail::allocation_journal(*session).empty()) {
      return false;
    }
  }
  const bool closed = tavern::close_database(*database).has_value();
  std::filesystem::remove(file);
  return closed;
}

}  // namespace

auto main() -> int {
  const auto bytes = encoded();
  return bytes && reuse_order_and_append(*bytes) &&
                 append_and_rejection_cases(*bytes) &&
                 stale_invalid_and_partial_rejection(*bytes)
             ? 0
             : 1;
}
