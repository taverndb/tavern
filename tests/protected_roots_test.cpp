#include "protected_roots.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <filesystem>
#include <unordered_set>

#include "database_internal.hpp"
#include "reachability.hpp"
namespace {
auto path(const char* s) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         (std::string("tavern-protected-") +
          std::to_string(static_cast<long long>(getpid())) + s);
}
auto size(const std::filesystem::path& p, off_t& n) -> bool {
  struct stat s{};
  return stat(p.c_str(), &s) == 0 ? (n = s.st_size, true) : false;
}
auto get64(const std::array<std::byte, tavern::page_size>& p, std::size_t o)
    -> std::uint64_t {
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < 8; ++i)
    v |= static_cast<std::uint64_t>(
             std::to_integer<unsigned long long>(p[o + i]))
         << (i * 8U);
  return v;
}
void put64(std::array<std::byte, tavern::page_size>& p, std::size_t o,
           std::uint64_t v) {
  for (std::size_t i = 0; i < 8; ++i)
    p[o + i] = static_cast<std::byte>(v >> (i * 8U));
}
auto checksum(const std::array<std::byte, tavern::page_size>& p)
    -> std::uint64_t {
  std::uint64_t v = 14695981039346656037ULL;
  for (std::size_t i = 20; i < p.size(); ++i) {
    v ^= std::to_integer<unsigned int>(p[i]);
    v *= 1099511628211ULL;
  }
  return v;
}
auto extent_corruption() -> bool {
  const auto p = path("-extent.db");
  std::filesystem::remove(p);
  auto d = tavern::create_database(p);
  if (!d) return false;
  tavern::PageId root = tavern::invalid_page_id;
  for (tavern::PersonId id = 0; id < 300; ++id) {
    tavern::PatronRecord record{};
    record.id = id;
    const auto next =
        tavern::directory_insert(database_impl(*d).store, root, record);
    if (!next) return false;
    root = *next;
  }
  if (!tavern::commit_root(database_impl(*d).store, database_impl(*d).commit,
                           root))
    return false;
  const auto trailing =
      tavern::append_page(database_impl(*d).store, tavern::PageType::data,
                          std::array<std::byte, 1>{std::byte{1}});
  std::array<std::byte, tavern::page_size> raw{};
  if (!trailing || *trailing != database_impl(*d).commit.extent ||
      pread(database_impl(*d).store.file_descriptor, raw.data(), raw.size(),
            static_cast<off_t>(root * tavern::page_size)) !=
          static_cast<ssize_t>(raw.size()))
    return false;
  std::size_t child = tavern::directory_fanout;
  for (std::size_t i = 0; i < tavern::directory_fanout; ++i)
    if (get64(raw, 21 + i * 8) != tavern::invalid_page_id) {
      child = i;
      break;
    }
  if (child == tavern::directory_fanout) return false;
  put64(raw, 21 + child * 8, *trailing);
  put64(raw, 12, checksum(raw));
  const bool rejected =
      pwrite(database_impl(*d).store.file_descriptor, raw.data(), raw.size(),
             static_cast<off_t>(root * tavern::page_size)) ==
          static_cast<ssize_t>(raw.size()) &&
      !tavern::scan_protected_directories(database_impl(*d).store) &&
      !tavern::scan_reachable_pages(database_impl(*d).store);
  const auto closed = tavern::close_database(*d);
  std::filesystem::remove(p);
  return rejected && closed.has_value();
}
}  // namespace
auto main() -> int {
  if (!extent_corruption()) return 1;
  const auto empty_path = path("-empty.db");
  std::filesystem::remove(empty_path);
  auto empty = tavern::create_database(empty_path);
  if (!empty) return 1;
  auto empty_scan =
      tavern::scan_protected_directories(database_impl(*empty).store);
  if (!empty_scan || empty_scan->roots.size() != 1 ||
      !empty_scan->directory_pages.empty() || !tavern::close_database(*empty))
    return 1;
  std::filesystem::remove(empty_path);
  const auto p = path("-roots.db");
  std::filesystem::remove(p);
  auto d = tavern::create_database(p);
  if (!d || !tavern::follow(*d, 1, 2)) return 1;
  const auto older = database_impl(*d).commit.root;
  if (!tavern::befriend(*d, 1, 3)) return 1;
  const auto newer = database_impl(*d).commit.root;
  off_t before = 0, after = 0;
  if (!size(p, before)) return 1;
  auto scan = tavern::scan_protected_directories(database_impl(*d).store);
  if (!scan || scan->roots.size() != 2 ||
      !scan->directory_pages.contains(older) ||
      !scan->directory_pages.contains(newer) ||
      scan->adjacency_descriptors.size() < 6 || !size(p, after) ||
      before != after)
    return 1;
  bool older_extent = false, newer_extent = false;
  for (const auto& a : scan->adjacency_descriptors) {
    older_extent = older_extent || a.committed_extent == scan->roots[0].extent;
    newer_extent = newer_extent || a.committed_extent == scan->roots[1].extent;
  }
  if (!older_extent || !newer_extent) return 1;
  const auto pages = scan->directory_pages;
  if (!tavern::close_database(*d)) return 1;
  auto store = tavern::open_page_store(p);
  auto reopened =
      store ? tavern::scan_protected_directories(*store)
            : std::expected<tavern::ProtectedDirectoryScan, tavern::Error>(
                  std::unexpected(tavern::Error::io_failure));
  if (!reopened || reopened->directory_pages != pages ||
      !tavern::close_page_store(*store))
    return 1;
  std::filesystem::remove(p);
  return 0;
}
