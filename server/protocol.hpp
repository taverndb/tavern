#pragma once

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <tavern/database.hpp>

namespace tavern::protocol {

// Text Protocol v1 uses one printable-ASCII request per LF-delimited frame.
// Requests are a command followed by one or two single-space-separated unsigned
// decimal PersonIds. Responses are LF-delimited `OK`, `BOOL`, `IDS`, or `ERR`
// records. The dispatcher is independent of page formats: Database is its only
// persistence-facing dependency.
inline constexpr std::size_t maximum_request_size = 4096;
inline constexpr std::size_t maximum_response_size = 65536;

// Parses and executes exactly one unframed request. Syntax failures and
// database operation failures are represented as protocol ERR strings; an
// expected error is reserved for failure to construct a response, such as
// allocation failure.
std::expected<std::string, Error> execute_command(Database& database,
                                                  std::string_view request);

// Produces one bounded IDS response. It is public to protocol tests, not a
// storage API.
std::expected<std::string, Error> format_id_list(std::span<const PersonId> ids);

}  // namespace tavern::protocol
