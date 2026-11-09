#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <filesystem>
#include <iostream>

#include "database_internal.hpp"
namespace {
using RawPage = std::array<std::byte, tavern::page_size>;
auto checksum(const RawPage& p) -> std::uint64_t {
  std::uint64_t v = 14695981039346656037ULL;
  for (std::size_t i = 20; i < p.size(); ++i) {
    v ^= std::to_integer<unsigned int>(p[i]);
    v *= 1099511628211ULL;
  }
  return v;
}
[[maybe_unused]] void put16(RawPage& p, std::size_t o, std::uint16_t v) {
  p[o] = static_cast<std::byte>(v);
  p[o + 1] = static_cast<std::byte>(v >> 8U);
}
[[maybe_unused]] void put32(RawPage& p, std::size_t o, std::uint32_t v) {
  for (std::size_t i = 0; i < 4; ++i)
    p[o + i] = static_cast<std::byte>(v >> (i * 8U));
}
void put64(RawPage& p, std::size_t o, std::uint64_t v) {
  for (std::size_t i = 0; i < 8; ++i)
    p[o + i] = static_cast<std::byte>(v >> (i * 8U));
}
[[maybe_unused]] auto read_page(int fd, tavern::PageId id, RawPage& p) -> bool {
  return pread(fd, p.data(), p.size(),
               static_cast<off_t>(id * tavern::page_size)) ==
         static_cast<ssize_t>(p.size());
}
[[maybe_unused]] auto write_page(int fd, tavern::PageId id, const RawPage& p)
    -> bool {
  return pwrite(fd, p.data(), p.size(),
                static_cast<off_t>(id * tavern::page_size)) ==
         static_cast<ssize_t>(p.size());
}
[[maybe_unused]] void update_checksums(RawPage& p) {
  std::uint64_t m = 14695981039346656037ULL;
  for (std::size_t i = 20; i < 52; ++i) {
    m ^= std::to_integer<unsigned int>(p[i]);
    m *= 1099511628211ULL;
  }
  put64(p, 52, m);
  put64(p, 12, checksum(p));
}
}  // namespace
auto main() -> int {
  const auto p = std::filesystem::temp_directory_path() /
                 ("tavern-recovery-" +
                  std::to_string(static_cast<long long>(getpid())) + ".db");
  std::filesystem::remove(p);
  auto d = tavern::create_database(p);
  if (!d || !tavern::follow(*d, 1, 2) || !tavern::befriend(*d, 1, 3)) return 1;
  const auto generation = database_impl(*d).commit.generation;
  const auto root = database_impl(*d).commit.root;
  if (!tavern::close_database(*d)) return 1;
  auto reopened = tavern::open_database(p);
  if (!reopened || database_impl(*reopened).commit.generation != generation ||
      database_impl(*reopened).commit.root != root ||
      !tavern::is_following(*reopened, 1, 2).value() ||
      !tavern::are_friends(*reopened, 1, 3).value())
    return 1;
  if (!tavern::follow(*reopened, 2, 3) || !tavern::close_database(*reopened))
    return 1;
  int fd = open(p.c_str(), O_RDWR);
  if (fd < 0) return 1;
  std::byte bad{0xAA};
  if (pwrite(fd, &bad, 1, 12) != 1 || close(fd) != 0) return 1;
  auto fallback = tavern::open_database(p);
  if (!fallback) return 1;
  if (!tavern::close_database(*fallback)) return 1;
  fd = open(p.c_str(), O_RDWR);
  if (fd < 0) return 1;
  if (pwrite(fd, &bad, 1, static_cast<off_t>(tavern::page_size + 12)) != 1 ||
      close(fd) != 0)
    return 1;
  if (tavern::open_database(p)) return 1;
  std::filesystem::remove(p);
  return 0;
}
