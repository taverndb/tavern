# Tavern Architecture

Tavern is a durable C++23 relationship graph database for applications that
need directed `FOLLOW` edges and symmetric `FRIEND` edges. A person is an
application-supplied unsigned 64-bit identifier. Tavern neither stores person
profiles nor allocates identifiers; it persists only relationships. Repeating
an addition, or removing a missing edge, is a successful no-op. Enumeration
order is unspecified.

The system is deliberately small: it provides an embedded library, an IPv4 TCP
server, and a TCP client. It does not provide authentication, authorization,
TLS, replication, transactions spanning multiple requests, or a repair utility.

## Components and boundaries

```text
application ── C++ API ──> libtavern ──> one locked database file
                                  ^
TCP client ─ Text Protocol v1 ─ tavern-server
```

- **`libtavern`** is the installed embedded library. Its public surface is
  `<tavern/database.hpp>` and it owns persistent storage, recovery, and graph
  operations.
- **`tavern-server`** opens an existing database and exposes the Text Protocol
  over IPv4 TCP. Its protocol and event-loop libraries are build-private;
  embedded consumers do not depend on them.
- **`tavern-cli`** is a standalone protocol client. It has no `libtavern` link
  dependency and never opens a database file.

Tests and benchmarks use a separate, non-installed support target. In
particular, the page-liveness index there is a reference/test mechanism; the
production allocator uses retained reachability-derived free-page pools.

## Persistent representation

A database is one page-aligned file of 4096-byte pages. Page IDs are physical
page indexes, so a page's offset is its ID multiplied by the page size. Pages 0
and 1 are reserved metadata slots; data pages start at 2.

Each page has a fixed little-endian header containing a magic value, format
version, page type, payload length, and checksum. The checksum covers the
entire payload area, including zero padding. Readers validate these fields
before interpreting typed contents.

```text
page 0: metadata slot A       page 1: metadata slot B
+----------------------+      +----------------------+
| generation, root,    |      | generation, root,    |
| committed extent     |      | committed extent     |
+----------------------+      +----------------------+
page 2 onward: immutable directory and adjacency pages
```

A metadata record names a directory root and a **committed extent**. The extent
is the highest exclusive page bound visible to that generation. The physical
file can be larger after unpublished appended writes, so traversal must respect
the metadata extent rather than assume every physical page is committed.

The directory maps a person ID to three adjacency descriptors: following,
followers, and friends. It is a 16-way hash trie. Each internal directory page
consumes four hash bits; leaves store complete records and handle hash
collisions by scanning their bounded record collections. Thus lookup cost is
proportional to visited trie levels plus leaf collision work, not unconditionally
constant time.

An adjacency descriptor contains a root ID, reference-tree height, and member
count. Adjacency leaves store Person IDs; reference pages store child page IDs.
The bounded-fanout tree allows traversal to stream pages and members without
loading the whole relationship set. Membership currently enumerates the tree,
and removal searches it depth-first; these operations are therefore linear in
the traversed adjacency structure. The extent-bounded traversal used for
reachability rejects malformed page types, invalid counts, cycles, and
references beyond the generation extent. Ordinary member iteration checks page
shape and the final descriptor count, but does not independently apply an extent
bound or a visited-page set.

## Copy-on-write publication and recovery

Mutations do not modify directory or adjacency pages that a committed root may
reach. They write replacements, copy the changed directory path, and retain
unchanged sibling pages by reference. Older metadata generations can therefore
continue to name their original trees.

```text
write replacement pages
          |
          v
fsync database file
          |
          v
write inactive metadata slot (new root, generation, extent)
          |
          v
fsync database file
          |
          v
new generation becomes the in-memory active generation
```

The inactive metadata slot is overwritten only after replacement pages have
been synchronized. On open, Tavern validates both slots independently and
selects the valid slot with the larger generation (ties select slot A). A crash
before new metadata becomes durable can recover the preceding generation; a
crash after it can recover the new generation. This is generation-level atomic
publication, assuming the OS, filesystem, and storage device honor `fsync`
ordering.

A metadata write or synchronization failure after its attempt is
**indeterminate**: durable publication may have occurred despite the error. The
open handle then enters `recovery_required` and refuses later graph operations;
it must be closed and reopened to establish a safe context. Recovery validates
metadata encoding and checks that the selected root decodes as a directory page,
but does not eagerly traverse every reachable descendant or establish every
structural invariant. Descendant corruption is reported when a later operation
traverses it. Tavern does not repair files or reconstruct ambiguous acknowledged
commits.

## Reachability and page reuse

Page reuse must preserve every generation that crash recovery might select. A
reachability scan starts from both valid metadata slots, traverses their
extent-bounded directory and adjacency trees, and marks each reachable physical
data page in a bitmap. Only unmarked pages are reuse candidates.

```text
valid metadata A ─┐
                  ├─> reachability bitmap ─> ordered free-page pool
valid metadata B ─┘
```

The production `FreePagePool` is an ordered snapshot tied to one database
instance, active metadata identity, committed extent, and physical page count.
A mutation allocation session consumes its candidates first and falls back to
appending pages when exhausted. It invalidates its pool after a failed reuse
write rather than returning a possibly changed page. Pages detached by a newly
committed root are not immediately added to a retained pool: a later scan must
first establish that neither protected metadata generation can reach them.

This conservative approach may retain reclaimable space longer than necessary,
but avoids overwriting pages that an older slot still protects. Reachability
scans and pools are in-memory state, not durable allocation metadata; restart
reconstructs safety from the file's metadata roots.

## Embedded API

`Database` is move-only and owns the open descriptor. Create requires a new
path; open recovers an existing file. Tavern takes a nonblocking advisory
exclusive `flock` for the open lifetime, so cooperating Tavern processes cannot
open the same file concurrently. Programs that ignore advisory locks are outside
that protection.

A successful mutation has been published durably through the protocol above.
Self-relationships fail. `follow(a, b)` is directional; `befriend(a, b)`
updates symmetric membership. Query methods are read-only. Streaming
`for_each_*` methods avoid a result vector and propagate callback errors;
vector-returning methods materialize the same unspecified order.

Destruction releases the descriptor but cannot report close errors. Applications
that require an observable close result should call `Database::close()`. A
Database instance does not provide synchronization for concurrent method calls.

## TCP request path

Text Protocol v1 carries one printable-ASCII request per LF-delimited frame.
The server removes an optional preceding CR, then strictly checks token count,
single-space tokenization, and overflow-safe unsigned decimal IDs. The protocol
layer maps database failures to stable `ERR` categories rather than exposing
storage-page details.

```text
stdin or argv
    |                 one complete request / one complete response
    v
 tavern-cli ─────────────────────────────────────────────> tavern-server
                                                               |
                                                               v
                                                        protocol dispatcher
                                                               |
                                                               v
                                                        Database operation
```

The dispatcher fully constructs a response before the server queues it for
transmission. This permits response-size validation before any bytes are sent;
ID lists that exceed the response limit produce an error rather than a partial
list. The CLI sends requests sequentially and reads through exactly one response
LF before issuing the next request. In interactive mode it keeps one connection;
`EXIT` and `QUIT` are local commands and are never transmitted. In one-shot mode
stdout contains the server response while diagnostics use stderr; protocol `ERR`
and transport failures return a nonzero exit status.

## Event-driven server

The server uses nonblocking sockets and native readiness notification:
`kqueue` read/write filters on macOS and BSD, or `epoll` masks on Linux. A small
wrapper presents their common operations. Each connection owns partial request,
input, output, framing, peer-close, and last-activity state.

Readiness does not imply a complete frame or complete write. The server retains
partial input, discards overlong frames through their LF delimiter, and retains
partial output until writable readiness permits another send. Per-connection
input and output caps, bounded accept batches, bounded request processing, and
bounded write bytes prevent one busy peer from monopolizing the loop. Connections
with no successful socket I/O for 30 seconds are closed.

The server multiplexes many connections but executes protocol dispatch and all
Database calls serially on its one event-loop thread. This avoids concurrent use
of a Database while also meaning a slow database operation delays other ready
requests. `SIGINT` and `SIGTERM` set a stop flag; normal loop exit closes the
listening socket, clients, and database. It does not promise to drain buffered
responses before shutdown.

## Reliability boundaries and tradeoffs

Tavern's main tradeoff is simple, conservative crash safety over maximal space
reuse or concurrency. Copy-on-write pages plus dual metadata slots avoid an
in-place write-ahead log and make recovery choose a whole generation. The cost is
extra write amplification, scans for future reuse, and retention of pages still
reachable from either metadata slot.

Durability relies on POSIX I/O and `fsync` behavior. Tavern does not model power
loss beyond its crash-boundary tests, storage-controller reordering contrary to
`fsync`, media failure, arbitrary file corruption, or hostile concurrent access.
The server intentionally has no authentication, encryption, or TLS. Operators
must supply appropriate network access controls and transport protection when
exposing it beyond a trusted network.
