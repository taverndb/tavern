#include "protocol.hpp"

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace {

auto path() -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         ("tavern-protocol-" +
          std::to_string(static_cast<long long>(getpid())) + ".db");
}

auto response(tavern::Database& database, std::string_view request,
              std::string_view expected) -> bool {
  const auto result = tavern::protocol::execute_command(database, request);
  return result && *result == expected;
}

auto contains(std::string_view response, tavern::PersonId id) -> bool {
  const std::string token = " " + std::to_string(id);
  return response.find(token) != std::string_view::npos;
}

auto parser_errors(tavern::Database& database) -> bool {
  std::string non_ascii{"FOLLOW 1 2"};
  non_ascii.push_back(static_cast<char>(0x80));
  const std::string oversized(tavern::protocol::maximum_request_size + 1U, 'X');
  return response(database, "", "ERR BAD_REQUEST\n") &&
         response(database, " FOLLOW 1 2", "ERR BAD_REQUEST\n") &&
         response(database, "FOLLOW 1 2 ", "ERR BAD_REQUEST\n") &&
         response(database, "FOLLOW  1 2", "ERR BAD_REQUEST\n") &&
         response(database, "FOLLOW\t1 2", "ERR BAD_REQUEST\n") &&
         response(database, non_ascii, "ERR BAD_REQUEST\n") &&
         response(database, "follow 1 2", "ERR UNKNOWN_COMMAND\n") &&
         response(database, "UNKNOWN 1", "ERR UNKNOWN_COMMAND\n") &&
         response(database, "FOLLOW 1", "ERR BAD_REQUEST\n") &&
         response(database, "FOLLOW 1 2 3", "ERR BAD_REQUEST\n") &&
         response(database, "FOLLOW +1 2", "ERR BAD_REQUEST\n") &&
         response(database, "FOLLOW 18446744073709551616 2",
                  "ERR BAD_REQUEST\n") &&
         response(database, oversized, "ERR REQUEST_TOO_LARGE\n");
}

auto response_boundaries() -> bool {
  std::vector<tavern::PersonId> ids(
      3121, std::numeric_limits<tavern::PersonId>::max());
  ids.front() = 999999;
  const auto exact = tavern::protocol::format_id_list(ids);
  if (!exact || exact->size() != tavern::protocol::maximum_response_size ||
      exact->back() != '\n') {
    return false;
  }
  ids.push_back(std::numeric_limits<tavern::PersonId>::max());
  const auto oversized = tavern::protocol::format_id_list(ids);
  return oversized && *oversized == "ERR RESPONSE_TOO_LARGE\n";
}

auto commands_and_durability() -> bool {
  const auto file = path();
  std::filesystem::remove(file);
  auto database = tavern::Database::create(file);
  if (!database || !parser_errors(*database) ||
      !response(*database, "FOLLOW 1 1", "ERR SELF_RELATION\n") ||
      !response(*database, "FOLLOW 18446744073709551615 2", "OK\n") ||
      !response(*database, "IS_FOLLOWING 18446744073709551615 2", "BOOL 1\n") ||
      !response(*database, "FOLLOW 1 2", "OK\n") ||
      !response(*database, "FOLLOW 1 2", "OK\n") ||
      !response(*database, "IS_FOLLOWING 2 1", "BOOL 0\n") ||
      !response(*database, "BEFRIEND 1 3", "OK\n") ||
      !response(*database, "ARE_FRIENDS 3 1", "BOOL 1\n") ||
      !response(*database, "IS_FOLLOWING 1 3", "BOOL 0\n") ||
      !response(*database, "MUTUAL_FRIENDS 1 2", "IDS 0\n") ||
      !response(*database, "BEFRIEND 2 3", "OK\n") ||
      !response(*database, "MUTUAL_FRIENDS 1 2", "IDS 1 3\n") ||
      !response(*database, "UNFOLLOW 1 2", "OK\n") ||
      !response(*database, "UNFOLLOW 1 2", "OK\n") ||
      !response(*database, "UNFRIEND 1 3", "OK\n") ||
      !response(*database, "UNFRIEND 1 3", "OK\n")) {
    return false;
  }
  const auto following =
      tavern::protocol::execute_command(*database, "FOLLOWING 1");
  const auto followers =
      tavern::protocol::execute_command(*database, "FOLLOWERS 2");
  const auto friends =
      tavern::protocol::execute_command(*database, "FRIENDS 2");
  if (!following || !followers || !friends || *following != "IDS 0\n" ||
      *followers != "IDS 1 18446744073709551615\n" || !contains(*friends, 3) ||
      !database->close() ||
      !response(*database, "IS_FOLLOWING 1 2", "ERR IO_ERROR\n") ||
      !response(*database, "FRIENDS 2", "ERR IO_ERROR\n")) {
    return false;
  }
  auto reopened = tavern::Database::open(file);
  const bool result =
      reopened && response(*reopened, "ARE_FRIENDS 2 3", "BOOL 1\n") &&
      response(*reopened, "FOLLOWING 18446744073709551615", "IDS 1 2\n") &&
      reopened->close();
  std::filesystem::remove(file);
  return result;
}

}  // namespace

auto main() -> int {
  return response_boundaries() && commands_and_durability() ? 0 : 1;
}
