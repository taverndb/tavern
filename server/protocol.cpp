#include "protocol.hpp"

#include <array>
#include <limits>
#include <new>

namespace tavern::protocol {
namespace {

constexpr std::string_view ok = "OK\n";
constexpr std::string_view bad_request = "ERR BAD_REQUEST\n";
constexpr std::string_view unknown_command = "ERR UNKNOWN_COMMAND\n";
constexpr std::string_view request_too_large = "ERR REQUEST_TOO_LARGE\n";
constexpr std::string_view response_too_large = "ERR RESPONSE_TOO_LARGE\n";

auto error_code(Error error) -> std::string_view {
  // The wire protocol intentionally coalesces storage-specific failures. It
  // gives clients stable operational categories without exposing page formats
  // or promising a one-to-one mapping for internal errors.
  switch (error) {
    case Error::self_relationship:
      return "SELF_RELATION";
    case Error::out_of_memory:
    case Error::block_capacity_exhausted:
    case Error::page_limit:
    case Error::directory_capacity_exhausted:
      return "RESOURCE_EXHAUSTED";
    case Error::io_failure:
      return "IO_ERROR";
    case Error::database_locked:
      return "DATABASE_LOCKED";
    case Error::recovery_required:
      return "RECOVERY_REQUIRED";
    case Error::commit_not_published:
      return "COMMIT_FAILED";
    case Error::invalid_page:
    case Error::invalid_metadata:
    case Error::directory_exists:
    case Error::directory_missing:
    case Error::invalid_directory:
    case Error::invariant_violation:
      return "DATABASE_ERROR";
  }
  return "DATABASE_ERROR";
}

auto error_response(Error error) -> std::string {
  return "ERR " + std::string(error_code(error)) + "\n";
}

auto decimal_size(PersonId value) -> std::size_t {
  std::size_t size = 1;
  while (value >= 10) {
    value /= 10;
    ++size;
  }
  return size;
}

auto valid_ascii(std::string_view request) -> bool {
  for (const char character : request) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte < 0x20 || byte > 0x7e) return false;
  }
  return true;
}

auto parse_person(std::string_view token) -> std::expected<PersonId, Error> {
  // Accept only the protocol's unsigned decimal spelling. The pre-multiply
  // bound rejects overflow instead of accepting a wrapped PersonId.
  if (token.empty()) return std::unexpected(Error::invalid_page);
  PersonId value = 0;
  for (const char character : token) {
    if (character < '0' || character > '9')
      return std::unexpected(Error::invalid_page);
    const PersonId digit = static_cast<PersonId>(character - '0');
    if (value > (std::numeric_limits<PersonId>::max() - digit) / 10)
      return std::unexpected(Error::invalid_page);
    value = value * 10 + digit;
  }
  return value;
}

auto make_id_response(std::size_t count, std::size_t digit_total,
                      const auto& enumerate)
    -> std::expected<std::string, Error> {
  const std::size_t prefix = 4 + decimal_size(count) + 1;
  if (count > maximum_response_size ||
      digit_total > maximum_response_size - prefix ||
      count > maximum_response_size - prefix - digit_total)
    return std::string(response_too_large);
  const std::size_t size = prefix + count + digit_total;
  if (size > maximum_response_size) return std::string(response_too_large);
  // Construct the whole frame before the server starts transmission. That makes
  // the length check atomic from the connection's perspective and lets ordinary
  // socket backpressure operate on complete protocol responses.
  try {
    std::string response;
    response.reserve(size);
    response += "IDS ";
    response += std::to_string(count);
    const auto appended = enumerate([&](PersonId person) -> auto {
      response.push_back(' ');
      response += std::to_string(person);
      return std::expected<void, Error>{};
    });
    if (!appended) return std::unexpected(appended.error());
    response.push_back('\n');
    return response;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::out_of_memory);
  }
}

template <typename Enumeration>
auto streaming_id_response(Enumeration enumerate)
    -> std::expected<std::string, Error> {
  std::size_t count = 0;
  std::size_t digit_total = 0;
  bool oversized = false;
  const auto counted = enumerate([&](PersonId person) -> auto {
    const std::size_t digits = decimal_size(person);
    if (count == std::numeric_limits<std::size_t>::max() ||
        digit_total > maximum_response_size ||
        digits > maximum_response_size - digit_total) {
      oversized = true;
      return std::expected<void, Error>(
          std::unexpected(Error::invariant_violation));
    }
    ++count;
    digit_total += digits;
    return std::expected<void, Error>{};
  });
  if (!counted && !oversized) return std::unexpected(counted.error());
  if (oversized) return std::string(response_too_large);
  return make_id_response(count, digit_total, enumerate);
}

template <typename Enumeration>
auto list_response(Enumeration enumerate) -> std::expected<std::string, Error> {
  const auto response = streaming_id_response(enumerate);
  return response ? response
                  : std::expected<std::string, Error>(
                        error_response(response.error()));
}

auto tokens(std::string_view request, std::size_t& count)
    -> std::expected<std::array<std::string_view, 3>, Error> {
  // Deliberately reject whitespace normalization, control bytes, empty tokens,
  // and more than two arguments. A request has one canonical token spelling,
  // which keeps command dispatch and machine-generated clients unambiguous.
  if (request.empty() || request.front() == ' ' || request.back() == ' ' ||
      !valid_ascii(request))
    return std::unexpected(Error::invalid_page);
  std::array<std::string_view, 3> result{};
  count = 0;
  std::size_t start = 0;
  while (start < request.size()) {
    const std::size_t end = request.find(' ', start);
    if (count == result.size()) return std::unexpected(Error::invalid_page);
    result[count++] = request.substr(start, end - start);
    if (end == std::string_view::npos) break;
    start = end + 1;
    if (start == request.size() || request[start] == ' ')
      return std::unexpected(Error::invalid_page);
  }
  return result;
}

auto people(const std::array<std::string_view, 3>& request, std::size_t count,
            std::size_t expected)
    -> std::expected<std::array<PersonId, 2>, Error> {
  if (count != expected + 1) return std::unexpected(Error::invalid_page);
  std::array<PersonId, 2> ids{};
  for (std::size_t index = 0; index < expected; ++index) {
    const auto id = parse_person(request[index + 1]);
    if (!id) return std::unexpected(id.error());
    ids[index] = *id;
  }
  return ids;
}

}  // namespace

auto format_id_list(std::span<const PersonId> ids)
    -> std::expected<std::string, Error> {
  std::size_t digits = 0;
  for (const PersonId id : ids) {
    const std::size_t size = decimal_size(id);
    if (digits > maximum_response_size || size > maximum_response_size - digits)
      return std::string(response_too_large);
    digits += size;
  }
  return make_id_response(ids.size(), digits,
                          [&](const auto& callback) -> auto {
                            for (const PersonId id : ids) {
                              const auto appended = callback(id);
                              if (!appended) return appended;
                            }
                            return std::expected<void, Error>{};
                          });
}

auto execute_command(Database& database, std::string_view request)
    -> std::expected<std::string, Error> {
  try {
    if (request.size() > maximum_request_size)
      return std::string(request_too_large);
    std::size_t count = 0;
    const auto parsed = tokens(request, count);
    if (!parsed) return std::string(bad_request);
    const std::string_view command = (*parsed)[0];
    // Parsing and dispatch happen before any operation. Database errors become
    // complete ERR frames; only response-construction failure escapes through
    // expected for the event loop to map to its allocation fallback.
    const auto mutation =
        [&](auto operation) -> std::expected<std::string, Error> {
      const auto ids = people(*parsed, count, 2);
      if (!ids) return std::string(bad_request);
      const auto result = operation((*ids)[0], (*ids)[1]);
      return result ? std::expected<std::string, Error>(std::string(ok))
                    : std::expected<std::string, Error>(
                          error_response(result.error()));
    };
    const auto boolean =
        [&](auto operation) -> std::expected<std::string, Error> {
      const auto ids = people(*parsed, count, 2);
      if (!ids) return std::string(bad_request);
      const auto result = operation((*ids)[0], (*ids)[1]);
      if (!result) return error_response(result.error());
      return std::string("BOOL ") + (*result ? "1\n" : "0\n");
    };
    if (command == "FOLLOW")
      return mutation(
          [&](PersonId a, PersonId b) -> std::expected<void, Error> {
            return database.follow(a, b);
          });
    if (command == "UNFOLLOW")
      return mutation(
          [&](PersonId a, PersonId b) -> std::expected<void, Error> {
            return database.unfollow(a, b);
          });
    if (command == "BEFRIEND")
      return mutation(
          [&](PersonId a, PersonId b) -> std::expected<void, Error> {
            return database.befriend(a, b);
          });
    if (command == "UNFRIEND")
      return mutation(
          [&](PersonId a, PersonId b) -> std::expected<void, Error> {
            return database.unfriend(a, b);
          });
    if (command == "IS_FOLLOWING")
      return boolean([&](PersonId a, PersonId b) -> std::expected<bool, Error> {
        return database.is_following(a, b);
      });
    if (command == "ARE_FRIENDS")
      return boolean([&](PersonId a, PersonId b) -> std::expected<bool, Error> {
        return database.are_friends(a, b);
      });

    const auto one_person = [&]() -> std::expected<PersonId, Error> {
      const auto ids = people(*parsed, count, 1);
      return ids ? std::expected<PersonId, Error>((*ids)[0])
                 : std::expected<PersonId, Error>(
                       std::unexpected(Error::invalid_page));
    };
    if (command == "FOLLOWING" || command == "FOLLOWERS" ||
        command == "FRIENDS") {
      const auto person = one_person();
      if (!person) return std::string(bad_request);
      if (command == "FOLLOWING")
        return list_response([&](const auto& callback) -> auto {
          return database.for_each_following(*person, callback);
        });
      if (command == "FOLLOWERS")
        return list_response([&](const auto& callback) -> auto {
          return database.for_each_follower(*person, callback);
        });
      return list_response([&](const auto& callback) -> auto {
        return database.for_each_friend(*person, callback);
      });
    }
    if (command == "MUTUAL_FRIENDS") {
      const auto ids = people(*parsed, count, 2);
      if (!ids) return std::string(bad_request);
      const auto result = database.mutual_friends((*ids)[0], (*ids)[1]);
      return result ? format_id_list(*result) : error_response(result.error());
    }
    return std::string(unknown_command);
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::out_of_memory);
  }
}

}  // namespace tavern::protocol
