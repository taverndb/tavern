#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "database_internal.hpp"
#include "reachability.hpp"
namespace {
auto point(int v) -> tavern::CrashPoint {
  return static_cast<tavern::CrashPoint>(v);
}
auto candidates(tavern::Database& d) -> bool {
  const std::array<std::byte, 1> p{std::byte{0x1A}};
  for (int i = 0; i < 4; ++i)
    if (!tavern::append_page(database_impl(d).store, tavern::PageType::data, p))
      return false;
  const auto s = tavern::scan_reachable_pages(database_impl(d).store);
  return s && s->unreachable_data_pages >= 4;
}
auto child(const char* p, int raw) -> int {
  auto d = tavern::open_database(p);
  if (!d) return 2;
  if (raw == static_cast<int>(tavern::CrashPoint::during_reusable_write))
    database_impl(*d).store.fail_reusable_write_after_for_test = 1;
  tavern::set_crash_point_for_test(point(raw));
  return tavern::unfollow(*d, 4, 5) ? 3 : 4;
}
auto run(const char* self, const std::filesystem::path& p, int raw) -> bool {
  auto d = tavern::create_database(p);
  if (!d || !tavern::follow(*d, 1, 2) || !tavern::befriend(*d, 1, 3) ||
      !tavern::follow(*d, 4, 5) || !candidates(*d))
    return false;
  const auto pages = database_impl(*d).store.page_count;
  if (!tavern::close_database(*d)) return false;
  const pid_t pid = fork();
  if (pid == 0) {
    execl(self, self, "child", p.c_str(), std::to_string(raw).c_str(),
          static_cast<char*>(nullptr));
    _exit(127);
  }
  int status = 0;
  if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
      WEXITSTATUS(status) != 99)
    return false;
  auto r = tavern::open_database(p);
  if (!r || database_impl(*r).store.page_count != pages ||
      !tavern::is_following(*r, 1, 2).value_or(false) ||
      !tavern::are_friends(*r, 1, 3).value_or(false))
    return false;
  const bool durable =
      raw == static_cast<int>(tavern::CrashPoint::after_metadata_sync);
  const auto removed = tavern::is_following(*r, 4, 5);
  const auto scan = tavern::scan_reachable_pages(database_impl(*r).store);
  const bool ok = removed && *removed == !durable &&
                  database_impl(*r).commit.generation == (durable ? 4U : 3U) &&
                  scan && tavern::close_database(*r);
  return ok;
}
}  // namespace
auto main(int argc, char** argv) -> int {
  if (argc == 4 && std::string(argv[1]) == "child")
    return child(argv[2], std::atoi(argv[3]));
  bool ok = true;
  for (const int raw :
       {static_cast<int>(tavern::CrashPoint::during_reusable_write),
        static_cast<int>(tavern::CrashPoint::after_data_sync),
        static_cast<int>(tavern::CrashPoint::after_metadata_sync)}) {
    const auto p = std::filesystem::temp_directory_path() /
                   ("tavern-unfollow-crash-" +
                    std::to_string(static_cast<long long>(getpid())) + "-" +
                    std::to_string(raw) + ".db");
    std::filesystem::remove(p);
    ok = ok && run(argv[0], p, raw);
    std::filesystem::remove(p);
  }
  return ok ? 0 : 1;
}
