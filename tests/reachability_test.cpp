#include "reachability.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>

#include "database_internal.hpp"
namespace {
auto path(const char* s) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         (std::string("tavern-reach-") +
          std::to_string(static_cast<long long>(getpid())) + s);
}
auto filesize(const std::filesystem::path& p, off_t& n) -> bool {
  struct stat s{};
  return stat(p.c_str(), &s) == 0 ? (n = s.st_size, true) : false;
}
auto snapshot(const tavern::PageStore& store, std::vector<std::byte>& bytes)
    -> bool {
  bytes.resize(static_cast<std::size_t>(store.page_count) * tavern::page_size);
  return pread(store.file_descriptor, bytes.data(), bytes.size(), 0) ==
         static_cast<ssize_t>(bytes.size());
}
auto collect(const tavern::PageStore& store, tavern::PageId root,
             tavern::PageId extent)
    -> std::expected<std::unordered_set<tavern::PageId>, tavern::Error> {
  std::unordered_set<tavern::PageId> pages;
  const auto add = [&](tavern::PageId id,
                       tavern::PageType) -> std::expected<void, tavern::Error> {
    pages.insert(id);
    return std::expected<void, tavern::Error>{};
  };
  const auto result = tavern::traverse_directory(
      store, root, extent, add,
      [&](const tavern::PatronRecord& r) -> std::expected<void, tavern::Error> {
        for (const auto& d : {r.following, r.followers, r.friends}) {
          const auto a =
              tavern::traverse_adjacency_pages(store, d, extent, add);
          if (!a)
            return std::expected<void, tavern::Error>(
                std::unexpected(a.error()));
        }
        return std::expected<void, tavern::Error>{};
      });
  return result
             ? std::expected<std::unordered_set<tavern::PageId>, tavern::Error>(
                   pages)
             : std::expected<std::unordered_set<tavern::PageId>, tavern::Error>(
                   std::unexpected(result.error()));
}
}  // namespace
auto corrupted_protected_adjacency() -> bool {
  const auto p = path("-corrupt-adjacency.db");
  std::filesystem::remove(p);
  auto d = tavern::create_database(p);
  if (!d || !tavern::follow(*d, 1, 2)) return false;
  const auto a = tavern::read_metadata(database_impl(*d).store,
                                       tavern::metadata_slot_a),
             b = tavern::read_metadata(database_impl(*d).store,
                                       tavern::metadata_slot_b);
  if (!a || !b) return false;
  const auto metadata = a->root != tavern::invalid_page_id ? a : b;
  if (metadata->root == tavern::invalid_page_id) return false;
  const auto pages = collect(database_impl(*d).store, metadata->root,
                             metadata->committed_page_count);
  if (!pages) return false;
  tavern::PageId leaf = tavern::invalid_page_id;
  for (const auto id : *pages) {
    const auto page = tavern::read_page(database_impl(*d).store, id);
    if (!page) return false;
    if (page->type == tavern::PageType::adjacency_leaf) {
      leaf = id;
      break;
    }
  }
  if (leaf == tavern::invalid_page_id || !tavern::close_database(*d))
    return false;
  const int descriptor = open(p.c_str(), O_RDWR);
  const off_t checksum_offset =
      static_cast<off_t>(leaf * tavern::page_size + 12);
  std::byte corrupt{};
  const bool corrupted =
      descriptor >= 0 && pread(descriptor, &corrupt, 1, checksum_offset) == 1 &&
      (corrupt ^= std::byte{0xFF},
       pwrite(descriptor, &corrupt, 1, checksum_offset) == 1) &&
      close(descriptor) == 0;
  if (!corrupted) return false;
  auto store = tavern::open_page_store(p);
  std::vector<std::byte> before, after;
  if (!store || !snapshot(*store, before)) return false;
  const auto scan = tavern::scan_reachable_pages(*store);
  const bool unchanged = snapshot(*store, after) && before == after;
  const auto closed = tavern::close_page_store(*store);
  std::filesystem::remove(p);
  return !scan && scan.error() == tavern::Error::invalid_page && unchanged &&
         closed.has_value();
}
auto shared_cow_pages() -> bool {
  const auto p = path("-shared.db");
  std::filesystem::remove(p);
  auto d = tavern::create_database(p);
  if (!d) return false;
  for (std::size_t i = 0; i <= tavern::adjacency_page_capacity; ++i)
    if (!tavern::follow(*d, 1, static_cast<tavern::PersonId>(1000U + i)))
      return false;
  if (!tavern::follow(*d, 1,
                      static_cast<tavern::PersonId>(
                          1001U + tavern::adjacency_page_capacity)))
    return false;
  const auto a = tavern::read_metadata(database_impl(*d).store,
                                       tavern::metadata_slot_a),
             b = tavern::read_metadata(database_impl(*d).store,
                                       tavern::metadata_slot_b);
  if (!a || !b || a->root == b->root) return false;
  const auto old = a->generation < b->generation ? a : b;
  const auto newer = a->generation < b->generation ? b : a;
  const auto po = collect(database_impl(*d).store, old->root,
                          old->committed_page_count),
             pn = collect(database_impl(*d).store, newer->root,
                          newer->committed_page_count);
  if (!po || !pn) return false;
  std::unordered_set<tavern::PageId> shared;
  for (const auto id : *po)
    if (pn->contains(id)) shared.insert(id);
  auto expected = *po;
  expected.insert(pn->begin(), pn->end());
  const auto metadata_a = *a, metadata_b = *b;
  off_t before = 0, after = 0;
  if (shared.empty() || !filesize(p, before)) return false;
  const auto scan = tavern::scan_reachable_pages(database_impl(*d).store);
  const auto profiled =
      tavern::profile_reachable_pages(database_impl(*d).store);
  const auto a_after = tavern::read_metadata(database_impl(*d).store,
                                             tavern::metadata_slot_a),
             b_after = tavern::read_metadata(database_impl(*d).store,
                                             tavern::metadata_slot_b);
  if (!scan || !profiled || !a_after || !b_after || !filesize(p, after) ||
      before != after || scan->reachable_data_pages != expected.size() ||
      scan->reachable_data_pages == po->size() + pn->size() ||
      profiled->scan.reachable != scan->reachable ||
      profiled->diagnostics.adjacency_memo_hits == 0 ||
      profiled->diagnostics.adjacency_memo_extent_relaxed_hits == 0 ||
      a_after->generation != metadata_a.generation ||
      a_after->root != metadata_a.root ||
      a_after->committed_page_count != metadata_a.committed_page_count ||
      b_after->generation != metadata_b.generation ||
      b_after->root != metadata_b.root ||
      b_after->committed_page_count != metadata_b.committed_page_count)
    return false;
  for (const auto id : shared)
    if (!scan->reachable[static_cast<std::size_t>(id)]) return false;
  for (tavern::PageId id = tavern::first_data_page;
       id < database_impl(*d).store.page_count; ++id)
    if (scan->reachable[static_cast<std::size_t>(id)] != expected.contains(id))
      return false;
  tavern::PageId shared_adjacency = tavern::invalid_page_id;
  for (const auto id : shared) {
    const auto page = tavern::read_page(database_impl(*d).store, id);
    if (!page) return false;
    if (page->type == tavern::PageType::adjacency_leaf ||
        page->type == tavern::PageType::adjacency_reference) {
      shared_adjacency = id;
      break;
    }
  }
  if (shared_adjacency == tavern::invalid_page_id ||
      !tavern::close_database(*d))
    return false;
  const int descriptor = open(p.c_str(), O_RDWR);
  const off_t checksum_offset =
      static_cast<off_t>(shared_adjacency * tavern::page_size + 12);
  std::byte corrupt{};
  const bool corrupted =
      descriptor >= 0 && pread(descriptor, &corrupt, 1, checksum_offset) == 1 &&
      ((corrupt ^= std::byte{0xFF}),
       pwrite(descriptor, &corrupt, 1, checksum_offset) == 1) &&
      close(descriptor) == 0;
  auto store = tavern::open_page_store(p);
  const auto corrupted_scan =
      store ? tavern::profile_reachable_pages(*store)
            : std::expected<tavern::ProfiledReachabilityScan, tavern::Error>(
                  std::unexpected(tavern::Error::io_failure));
  const auto closed = store ? tavern::close_page_store(*store)
                            : std::expected<void, tavern::Error>(
                                  std::unexpected(tavern::Error::io_failure));
  std::filesystem::remove(p);
  return corrupted && !corrupted_scan && closed.has_value();
}
auto two_generations() -> bool {
  const auto p = path("-generations.db");
  std::filesystem::remove(p);
  auto d = tavern::create_database(p);
  if (!d || !tavern::follow(*d, 1, 2) || !tavern::befriend(*d, 3, 4))
    return false;
  const auto a = tavern::read_metadata(database_impl(*d).store,
                                       tavern::metadata_slot_a),
             b = tavern::read_metadata(database_impl(*d).store,
                                       tavern::metadata_slot_b);
  if (!a || !b || a->root == b->root) return false;
  off_t before = 0, after = 0;
  if (!filesize(p, before)) return false;
  const auto old = a->generation < b->generation ? a : b;
  const auto newer = a->generation < b->generation ? b : a;
  const auto po = collect(database_impl(*d).store, old->root,
                          old->committed_page_count),
             pn = collect(database_impl(*d).store, newer->root,
                          newer->committed_page_count);
  if (!po || !pn) return false;
  auto expected = *po;
  expected.insert(pn->begin(), pn->end());
  bool old_only = false;
  for (auto id : *po) old_only = old_only || !pn->contains(id);
  const auto scan = tavern::scan_reachable_pages(database_impl(*d).store);
  if (!scan || scan->reachable_data_pages != expected.size() || !old_only ||
      !filesize(p, after) || before != after)
    return false;
  for (tavern::PageId id = tavern::first_data_page;
       id < database_impl(*d).store.page_count; ++id)
    if (scan->reachable[static_cast<std::size_t>(id)] != expected.contains(id))
      return false;
  const bool metadata_clear = !scan->reachable[0] && !scan->reachable[1];
  tavern::close_database(*d);
  std::filesystem::remove(p);
  return metadata_clear;
}
auto main() -> int {
  if (!corrupted_protected_adjacency() || !shared_cow_pages() ||
      !two_generations())
    return 1;
  const auto empty_path = path("-empty.db");
  std::filesystem::remove(empty_path);
  auto empty = tavern::create_database(empty_path);
  auto empty_scan =
      empty ? tavern::scan_reachable_pages(database_impl(*empty).store)
            : std::expected<tavern::ReachabilityScan, tavern::Error>(
                  std::unexpected(tavern::Error::io_failure));
  if (!empty_scan || empty_scan->physical_data_pages != 0 ||
      empty_scan->reachable_data_pages != 0 ||
      empty_scan->unreachable_data_pages != 0 ||
      !tavern::close_database(*empty))
    return 1;
  std::filesystem::remove(empty_path);
  const auto p = path("-live.db");
  std::filesystem::remove(p);
  auto d = tavern::create_database(p);
  if (!d || !tavern::follow(*d, 1, 2)) return 1;
  off_t before = 0, after = 0;
  if (!filesize(p, before)) return 1;
  const auto known_root = database_impl(*d).commit.root;
  auto exact = collect(database_impl(*d).store, known_root,
                       database_impl(*d).commit.extent);
  if (!exact || !exact->contains(known_root) || exact->contains(0) ||
      exact->contains(1) || exact->size() < 3)
    return 1;
  auto scan = tavern::scan_reachable_pages(database_impl(*d).store);
  if (!scan || scan->reachable_data_pages == 0 ||
      scan->reachable.size() != database_impl(*d).store.page_count ||
      scan->reachable[0] || scan->reachable[1] || !filesize(p, after) ||
      before != after)
    return 1;
  if (!scan->reachable[static_cast<std::size_t>(known_root)]) return 1;
  const std::array<std::byte, 1> payload{std::byte{1}};
  const auto abandoned = tavern::append_page(database_impl(*d).store,
                                             tavern::PageType::data, payload);
  if (!abandoned) return 1;
  auto with_abandoned = tavern::scan_reachable_pages(database_impl(*d).store);
  if (!with_abandoned ||
      with_abandoned->reachable[static_cast<std::size_t>(*abandoned)] ||
      with_abandoned->unreachable_data_pages == 0)
    return 1;
  if (!tavern::close_database(*d)) return 1;
  auto store = tavern::open_page_store(p);
  auto reopened = store
                      ? tavern::scan_reachable_pages(*store)
                      : std::expected<tavern::ReachabilityScan, tavern::Error>(
                            std::unexpected(tavern::Error::io_failure));
  if (!reopened || reopened->reachable != with_abandoned->reachable ||
      !tavern::close_page_store(*store))
    return 1;
  std::filesystem::remove(p);
  return 0;
}
