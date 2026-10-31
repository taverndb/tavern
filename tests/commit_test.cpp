#include "commit.hpp"

#include <unistd.h>

#include <filesystem>

#include "directory.hpp"
auto main() -> int {
  auto p = std::filesystem::temp_directory_path() /
           ("tavern-commit-" +
            std::to_string(static_cast<long long>(getpid())) + ".db");
  std::filesystem::remove(p);
  auto s = tavern::create_page_store(p);
  if (!s) return 1;
  auto c = tavern::initialize_commit_context(*s);
  if (!c) return 1;
  tavern::PatronRecord first{};
  first.id = 1;
  auto root = tavern::directory_insert(*s, tavern::invalid_page_id, first);
  if (!root || !tavern::commit_root(*s, *c, *root)) return 1;
  if (c->generation != 1 || c->active_slot != tavern::metadata_slot_b) return 1;
  auto old = c->root;
  if (!tavern::commit_root(*s, *c, old) || c->generation != 1) return 1;
  s->fail_data_sync_for_test = true;
  if (tavern::commit_root(*s, *c, old + 1)) return 1;
  s->fail_data_sync_for_test = false;
  s->fail_metadata_write_for_test = true;
  tavern::PatronRecord second{};
  second.id = 2;
  auto next = tavern::directory_insert(*s, old, second);
  if (!next || tavern::commit_root(*s, *c, *next) || !c->recovery_required)
    return 1;
  if (tavern::commit_root(*s, *c, *next)) return 1;
  tavern::close_page_store(*s);
  std::filesystem::remove(p);
  return 0;
}
