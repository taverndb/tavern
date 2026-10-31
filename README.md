# Tavern 0.1.0

Tavern is a durable C++23 relationship graph database. It supports an embedded
library mode and a standalone TCP server mode, with directed `FOLLOW`
relationships and symmetric `FRIEND` relationships.

- `FOLLOW(a, b)` means `a` follows `b`; it does not imply `b` follows `a`.
- `FRIEND(a, b)` is symmetric.
- Self-relationships are rejected. Repeating a mutation, or removing a missing
  relationship, succeeds as a no-op.
- Enumeration order is unspecified.

## Architecture

For an implementation-level overview of storage, recovery, the API, protocol,
and event loop, see [the architecture document](docs/architecture.md).

## Requirements and build

Tavern requires CMake 3.20+, a C++23 compiler, and a POSIX system with
`flock`, fixed-size file I/O, and `fsync`. The v0.1.0 server supports IPv4.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /desired/prefix
```

Equivalent presets keep Debug, Release, and RelWithDebInfo builds separate:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

## Releases and continuous integration

GitHub Actions checks formatting and runs the Debug and Release test suites on
Linux and macOS for pushes and pull requests. Pushing a version tag such as
`v0.1.0` runs the Release build and tests on both platforms, then publishes the
installed artifacts as `.tar.gz` downloads on that tag's GitHub Release page.
Archive names include the runner-reported operating system and architecture.

## Support Tavern

Donations are accepted as native BNB on BNB Smart Chain Mainnet (chain ID 56).
Verify the receiving address and network before sending funds:

[`0xcF69E336cE485d691f07421126B80552d3C6AE6b` on BscScan](https://bscscan.com/address/0xcF69E336cE485d691f07421126B80552d3C6AE6b)

Only native BNB is accepted. Transfers are irreversible and donations are not
tax-deductible.

## Embedded-library mode

The installed public header is only `<tavern/database.hpp>`.

```cmake
find_package(Tavern CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE Tavern::tavern)
```

Create a database explicitly, perform durable mutations, and explicitly close
it when close errors must be observed:

```cpp
#include <tavern/database.hpp>

auto database = tavern::Database::create("social.db");
if (!database) return 1;
if (!database->follow(1, 2) || !database->befriend(1, 3)) return 1;

const auto follows = database->is_following(1, 2);
const auto friends = database->friends(1);
if (!follows || !*follows || !friends) return 1;

if (!database->close()) return 1;
```

`Database` is move-only and holds a nonblocking advisory exclusive lock for
its open lifetime. A second Tavern process opening the same underlying file
returns `Error::database_locked`. Programs that ignore advisory locks are not
protected by Tavern.

## Standalone-server mode

The installed `tavern-server` server opens an existing database; it never
creates one implicitly. The server and its text protocol are private
implementation components: embedded consumers link only `Tavern::tavern` and
require no server implementation dependency.

```sh
tavern-server --database social.db [--host 127.0.0.1] [--port 7373]
```

`--port 0` requests an ephemeral port, which is printed at startup. `--host`
accepts any IPv4 bind address; use `0.0.0.0` to listen on every IPv4 interface.
The default is `127.0.0.1`. Stop the server with `SIGINT` or `SIGTERM`; it
closes the listening socket, active client, and database.

The server is single-threaded and uses native `kqueue` (macOS/BSD) or `epoll`
(Linux) to serve multiple nonblocking client connections. Database operations
remain serial. Connections can issue multiple requests; idle receive and
blocked send operations time out after 30 seconds, after which the client is
closed.

Tavern has **no authentication, authorization, encryption, or TLS**. Operators
exposing it beyond a trusted network are responsible for firewalling, access
control, and transport security.

## TCP CLI

`tavern-cli` is a standalone Text Protocol v1 client; it does not open database
files or link against the embedded library.

```sh
tavern-cli [--host 127.0.0.1] [--port 7373]
tavern-cli [--host 127.0.0.1] [--port 7373] FOLLOW 1 2
```

With no command arguments it reads one command per standard-input line over a
persistent connection and prints each server response. `EXIT` and `QUIT` end
interactive mode locally. In single-command mode, a server `ERR` response and
transport failures return a nonzero exit status while preserving the response
on standard output.

## Text protocol v1

One ASCII request is sent per LF- or CRLF-terminated line. Tokens are separated
by single ASCII spaces; no leading/trailing spaces, tabs, quoting, or comments
are accepted. Person IDs are decimal unsigned 64-bit integers. Requests are at
most 4096 bytes excluding their line terminator. Every complete request
receives exactly one LF-terminated response. Complete responses are at most
65536 bytes.

Commands:

```text
FOLLOW <from> <to>          UNFOLLOW <from> <to>
BEFRIEND <a> <b>            UNFRIEND <a> <b>
IS_FOLLOWING <from> <to>    ARE_FRIENDS <a> <b>
FOLLOWING <person>          FOLLOWERS <person>
FRIENDS <person>            MUTUAL_FRIENDS <a> <b>
```

Responses:

```text
OK
BOOL 0
BOOL 1
IDS <count> [<person> ...]
ERR <code>
```

An ID list that would exceed the response limit returns
`ERR RESPONSE_TOO_LARGE` rather than a partial list. An oversized request is
discarded through its next LF and returns `ERR REQUEST_TOO_LARGE`; the
connection remains usable. Malformed and unknown requests also leave the
connection usable.

Protocol error codes are:

```text
BAD_REQUEST           UNKNOWN_COMMAND        REQUEST_TOO_LARGE
RESPONSE_TOO_LARGE    SELF_RELATION          RESOURCE_EXHAUSTED
IO_ERROR              DATABASE_LOCKED        RECOVERY_REQUIRED
COMMIT_FAILED         DATABASE_ERROR
```

Example:

```text
C: FOLLOW 1 2
S: OK
C: IS_FOLLOWING 1 2
S: BOOL 1
C: FOLLOWING 1
S: IDS 1 2
```

## Durability and recovery

Each successful relationship mutation is one durable atomic commit. Tavern
synchronizes newly written data before publishing a new metadata generation,
and keeps two alternating protected metadata slots. Existing tests cover normal
close/reopen, controlled process crashes at commit boundaries, metadata/root
corruption detection, and I/O failures.

Durability depends on the operating system, filesystem, and storage device
honoring POSIX `fsync` ordering. Tavern does not simulate power loss,
storage-controller write reordering, media faults, or arbitrary full-file
corruption. It does not automatically repair a database.

If a metadata publication result is indeterminate, the live handle enters
`Error::recovery_required` and refuses later graph operations. Stop the server
or close the application, preserve a copy of the database, and inspect or
restore it using operational procedures. Reopening may select the latest valid
metadata generation, but Tavern v0.1.0 provides no repair tool and makes no
claim that ambiguous acknowledged commits can be reconstructed.

## File compatibility

The current internal page/file format version is **2**. Before 1.0, Tavern does
not guarantee migration, downgrade, source, or binary compatibility across
releases. Unsupported or corrupt format metadata is rejected on open. Back up
database files before upgrading Tavern. v0.1.0 has no automatic migration or
repair utility.
