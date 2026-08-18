# Remote client/server architecture plan

Status: implemented and validated by the remote client/server PR stack; the
PR #119 review remediation completed on 2026-08-01. Amended 2026-08-16: the
production deployment transport is now the server's stdin/stdout carried over
an ssh session started by the client (`amrexplorer --ssh`, `amrexplorer-server
--stdio`); the loopback TCP listener and the `--connect`/SSH-tunnel workflow
it served remain as the test and tools transport. Sections below note the
amendment where it changes them.

The implementation follows the design below: a shared local/remote
dataset-session boundary, viewport- and page-bounded queries, a verified
FlatBuffers protocol and portable framed transport, a concurrent loopback
server, a single-reader client, remote Qt/CLI workflows, remote sequences and
prefetch, particles, clipped grid geometry, range availability, cache control,
cancellation, reconnect/reopen coverage, a headless `remote` preset,
installation and user documentation, and production protocol and Qt smoke
tests.

## 1. Objective

Promote the successful `prototypes/flatbuffers_wire` experiment into a
production client/server architecture that lets the existing Qt application
run locally while all plotfile metadata and data access happens on a remote
machine.

The production path must preserve the current local-file workflow and the
existing demand-driven query behavior. Remote use will look like:

```text
local Qt UI
    |
    | viewport-bounded view requests
    v
remote client ---- framed FlatBuffers over a Channel ---- headless server
                   (deployment: ssh stdio;                |
                    tests/tools: loopback TCP)            v
                                                   PlotfileDataset
                                                   view planning
                                                   clipped extraction
```

The supported deployment model (amended 2026-08-16): the client starts
`amrexplorer-server --stdio` on the remote machine as an ssh command and
speaks the framed protocol over that command's stdin/stdout -- the git/sftp
model. Nothing listens on either machine, no forwarding permission is needed,
the session works through `ProxyJump` bastions, and the server's lifetime is
bounded by the ssh session: end-of-stream ends it. SSH remains responsible for
authentication, encryption, host verification, and optional transport
compression. The loopback TCP listener remains the transport for tests and
in-process tooling.

## 2. Prototype findings to retain

The prototype established that the right remote boundary is above block I/O,
but its raw-plane response is only a transport proof:

- the server can open an actual plotfile and execute the existing
  `SliceQuery`;
- the client can receive typed numeric data and continue to apply palettes,
  ranges, logarithmic mapping, contours, and other 1-D/2-D presentation
  locally;
- a length-prefixed FlatBuffer with a file identifier, protocol version,
  request ID, payload union, frame-size limit, and verifier is a workable
  protocol foundation;
- the server can remain independent of Qt;
- build-time generation from one checked-in schema works with the current
  CMake build.

The prototype is not production-ready because it stops after one slice and
one connection. It does not support the catalog and view-local metadata needed
by the UI, line plots, the Dataset window, multiple datasets, multiple
outstanding requests, cancellation, orderly close, reconnect behavior, or
application integration. It also serializes a complete `ScalarPlane`;
production messages must instead contain no more data than is necessary to
rasterize the current view.

## 3. Scope

### In scope

- A reusable dataset-session abstraction used by the Qt application for both
  local and remote datasets.
- Production FlatBuffers schema, codec, framing, client connection, server
  session, and server executable.
- Lightweight dataset catalog metadata plus view-local metadata returned with
  each request.
- Viewport-bounded 1-D and 2-D view queries and visible Dataset-window pages.
- Multiple datasets and multiple outstanding requests on one connection.
- Cooperative cancellation using the existing `StopToken` model.
- Deterministic dataset close, connection shutdown, disconnect reporting, and
  manual reconnect/reopen.
- Local plotfiles, remote plotfiles, and local or remote plotfile sequences.
- Loopback-only server operation designed for SSH port forwarding.
- Unit, integration, end-to-end, and Qt smoke coverage.
- Build, installation, command-line, and user documentation.
- Removal of the prototype after production equivalence is demonstrated.

### Out of scope for protocol 1.0

- Directly exposing the server on an untrusted network.
- Application-level authorization or TLS. (A mandatory per-session access
  token *is* enforced — see §5.3 — so that a shared loopback interface does not
  let other local users on the server host connect through the owning user's
  account. It is not user authentication or transport encryption; SSH still
  provides those.)
- Remote filesystem browsing. (Added in protocol 1.1, 2026-08: a bounded,
  directories-only listing -- see §5.4 and §9.2.)
- Server-side rendering, palettes, contours, glyph generation, or image
  export for protocol 1.0.
- Shared datasets or cache state between separate client connections.
- Automatic retry of interrupted requests.
- Application-level compression of view data.
- Chunked or streaming query results.
- 3-D volume field data or arbitrary volume geometry on the wire. The compact
  AMR box hierarchy is an exception: it is catalog metadata used to draw the
  3-D wireframe. Volume rendering (protocol 1.2) returns server-rendered
  frames rather than transferring a volume.

These exclusions keep the first production protocol narrow while retaining
the validated transport mechanisms from the prototype. They do not prevent
additive protocol extensions.

## 4. Proposed architecture

### 4.1 Dataset session boundary

Introduce `amrvis::DatasetSession`, a high-level interface representing one
open dataset. The Qt layer will depend on this interface rather than directly
on `PlotfileDataset`.

The interface will expose:

- dataset ID;
- immutable `DatasetCatalog` containing only dataset-wide information needed
  before a view is requested;
- `MetadataReadMetrics` and file-version text;
- `requestView(ViewDataRequest, StopToken)` for 1-D and 2-D rasterizable
  views;
- `requestDatasetPage(DatasetPageRequest, StopToken)` for the visible portion
  of the Dataset window;
- a current cache-metrics snapshot;
- `clearUnpinnedCache()`;
- a best-effort, idempotent close operation.

Two implementations will provide the same behavior:

- `LocalDatasetSession` owns a `PlotfileDataset` and invokes the existing
  queries through a viewport-bounded planning and clipping layer.
- `RemoteDatasetSession` owns a remote dataset handle and sends the equivalent
  operations through a shared `WireConnection`.

This boundary avoids making the remote client emulate block reads. It also
keeps AMR composition, sampling, cache policy, and file parsing on the machine
that owns the data. Internal storage reads may load whole intersecting blocks,
but the server clips their values and geometry before serialization.

The proposed dependency direction is:

```text
Amrvis::core
    ^
Amrvis::io <- Amrvis::query
                    ^
              Amrvis::data
                    ^
              Amrvis::remote
                    ^
                Qt client
```

`Amrvis::data` will contain the session interface and local implementation.
`Amrvis::remote` will add FlatBuffers and networking without making the local
data/query libraries depend on either.

### 4.2 View planning and bounded extraction

`src/qt/DatasetExtract.hpp` currently reads blocks directly from
`PlotfileDataset`. Move this non-Qt operation and its result types into the new
data layer, then put all slice, line, and Dataset-window operations behind a
shared view planner.

Every `ViewDataRequest` will describe what the client can actually display:

- dataset ID and field;
- view kind, selected fields, and components;
- visible physical bounds and slice or line definition;
- viewport pixel width and height;
- sampling/interpolation mode and its required halo;
- requested overlays and the view-local geometry they require;
- cancellation token.

The planner will determine the finest useful sampling for that viewport,
identify contributing AMR cells, include only the interpolation halo needed
at its edges, and clip coverage and grid geometry to the visible bounds.
Repeated cells that cannot affect any output pixel must not be serialized.

`DatasetPageRequest` will similarly identify the visible table region and a
strict row/column or cell-count limit. Its response will retain the current
cell-index bounds, values, coverage mask, minimum/maximum, slice index, and
truncation flags only for that page. Scrolling or changing the region issues a
new request; an unrestricted whole-level extraction is not a remote
operation.

The Qt view and `DatasetWindow` will consume these owning results through
`DatasetSession`, so local and remote behavior is equivalent at the visible
view boundary.

### 4.3 Qt integration

Replace `std::shared_ptr<PlotfileDataset>` in `MainWindow`,
`InitialSliceResult`, `DatasetRequest`, and helper functions with
`std::shared_ptr<DatasetSession>`.

The following operations will be routed through the session:

- initial and refreshed slices;
- contour source slices;
- vector-component slices;
- line plots;
- Dataset-window extraction;
- cache metrics and cache clearing;
- plotfile-sequence frame load and prefetch.

For 1-D and 2-D views, presentation stays local. `ScalarRenderer`, contour
extraction, vector-glyph generation, range selection, image composition, and
export consume the bounded returned cells and do not move to the server.
3-D volume rendering (protocol 1.2) is the deliberate exception: the server
samples the field into a bounded grid, ray-casts it with the client's camera
and transfer-function lookup, and returns a viewport-sized frame; it never
sends volume field data or arbitrary volume geometry.

Opening a dataset will be refactored so the metadata and session are created
once. The current local flow reads metadata, then constructs
`PlotfileDataset` and reads it again for the initial slice. The unified
session factory will return the open session and its metadata together, for
both local and remote paths.

### 4.4 Dataset locations and connection ownership

Add a value type that distinguishes:

- local path;
- remote endpoint plus server-visible path.

One `MainWindow` owns at most one active connection -- the ssh session's.
Dataset sessions and prefetched sequence frames from that window share the
connection (its receive thread multiplexes responses by request ID).
Independent top-level windows use independent connections, matching their
existing independent dataset/cache state.

Remote dataset handles are scoped to a single server connection. After a
disconnect they are invalid and are never silently reused on a new
connection.

## 5. Wire protocol 1.0

### 5.1 Framing

Retain the prototype framing:

- four-byte unsigned payload length in network byte order;
- one non-empty FlatBuffer body;
- `AVR2` FlatBuffers file identifier;
- a configurable hard frame limit, defaulting to 128 MiB;
- exact-read and exact-write loops that handle interruption and partial I/O;
- buffer verification before any generated accessor is used.

The client and server will reject zero-length, oversized, corrupt, wrongly
identified, or unverifiable frames before dispatch. Length arithmetic,
vector-size multiplication, output dimensions, cache budgets, string lengths,
dataset counts, and outstanding-request counts will also be bounded before
allocation.

### 5.2 Envelope and version policy

Every message will retain:

- protocol major version and minor version;
- nonzero request ID;
- typed payload union.

Protocol policy:

- a major change may break compatibility;
- a minor version change must be additive;
- the handshake selects a minor version supported by both peers;
- the server only sends messages valid for the negotiated version;
- unknown or invalid client requests receive a typed error when the envelope
  is otherwise valid;
- malformed framing, failed verification, duplicate live request IDs, or
  messages sent before the handshake close the connection.

Packed messages are transient network data, not files. No persistence or
cross-major migration path is promised.

### 5.3 Handshake and capabilities

The first request must be `HelloRequest`. It will carry:

- client name and software version;
- supported protocol major version and minor version range;
- maximum accepted frame size;
- the session access token printed by the server at startup;
- supported optional capabilities.

The server compares the token against the one it generated (constant-time,
so a rejection does not leak how many bytes matched). A missing or wrong
token is answered with an `Unauthorized` error and the connection is closed
before any dataset request is served.

`HelloResponse` will return:

- server name and software version;
- selected protocol version;
- negotiated maximum frame size;
- enabled capabilities;
- server worker and resource limits useful for diagnostics.

Protocol 1.0 will define capabilities even if none are optional initially.
This gives compression or streaming a compatible negotiation point later.

### 5.4 Request and response messages

The production schema will cover:

| Request | Successful response | Purpose |
|---|---|---|
| `HelloRequest` | `HelloResponse` | Negotiate the session |
| `OpenDatasetRequest` | `DatasetOpened` | Open a path and return catalog metadata |
| `CloseDatasetRequest` | `DatasetClosed` | Release one remote handle |
| `ViewDataRequest` | `ViewDataResponse` | Return cells needed for one 1-D/2-D viewport |
| `DatasetPageRequest` | `DatasetPageResponse` | Populate one visible Dataset-window page |
| `ClearCacheRequest` | `CacheState` | Clear unpinned blocks |
| `CancelRequest` | `CancelAcknowledged` | Request cancellation by request ID |
| `PingRequest` | `PongResponse` | Explicit health check |
| `ListDirectoryRequest` (1.1) | `DirectoryListing` | List a server directory's subdirectories, marking plotfiles |
| `RenderedFrameRequest` (1.2) | `RenderedFrameResponse` | Render one volume frame on the server: camera, range, transfer lookup and voxel budget in; premultiplied pixels, the range used and sampling metrics out |
| any request | `ErrorResponse` | Typed terminal failure |

Every ordinary request has exactly one terminal response with the same
request ID. Responses may arrive in a different order from requests.

`CancelRequest` has its own request ID and names a target request ID. The
target request still receives its own terminal response, normally a
`Cancelled` error. If completion wins the race, the target's normal response
is valid and the cancellation acknowledgment reports that it was too late.

### 5.5 Metadata representation

`DatasetOpened` will carry only dataset-wide catalog metadata needed before a
view is requested:

- dimension, finest level, time, coordinate system, and physical domain;
- field names, component counts, centering, and component names;
- level numbers, steps, refinement ratios, cell sizes, and level domains;
- format/file-version text and metadata-read metrics;
- initial cache state.

It will not carry ghost extents, per-block statistics, FAB locations, or other
storage metadata. It does carry each AMR box's index bounds, solely to draw the
3-D wireframe. Metadata needed for 2-D grid overlays, coverage, or sampling is
computed for the current view and returned in that view's response.
Filesystem-specific fields remain on the server.

Decoders will validate semantic invariants after FlatBuffers verification,
including vector lengths, level counts, box bounds, field indices, and
metadata consistency.

### 5.6 Query results

`ViewDataRequest` will carry enough information to prove and enforce the
response bound:

- view kind and requested fields/components;
- visible physical bounds or visible line interval;
- slice axis and position where applicable;
- viewport pixel dimensions;
- sampling/interpolation mode;
- requested view-local overlays.

`ViewDataResponse` will carry only data that can contribute to rasterizing
that request:

- clipped contributing cell values for the requested fields/components;
- cell bounds or sample positions needed for client-side rasterization;
- validity and source-level information for those cells;
- the minimal interpolation halo at visible edges;
- clipped AMR coverage or grid-overlay geometry when requested;
- query metrics, encoded-byte counts, and current server cache state.

For a line view, the server will return a pixel-bounded representation. Smooth
or point-sampled modes may return at most one representative sample per
horizontal output pixel; modes that must preserve extrema may return a bounded
minimum/maximum envelope per pixel. Native-resolution full-axis arrays are
not wire payloads.

`DatasetPageResponse` will carry only the requested visible table page and its
strictly bounded values, masks, indices, and truncation information. It will
never carry a whole level.

The protocol must not expose `FabBlock`, whole-level arrays, unrestricted
dataset extracts, complete native-resolution lines, full-volume cells, or
arbitrary volume geometry. The AMR box hierarchy is permitted as compact
wireframe metadata. The server may overfetch storage blocks internally, but
must clip values, masks, coverage, and other geometry before serialization.

The client copies verified FlatBuffers vectors into the existing owning
view-result types before releasing the receive buffer. This preserves the
current lifetime model and keeps FlatBuffers-generated types out of the UI and
query APIs.

3-D volume rendering (protocol 1.2) uses the distinct
`RenderedFrameRequest`/`RenderedFrameResponse` pair. The request carries the
orthographic camera, the physical region, the output size, an optional
explicit range (or the request that the server resolve the "Visible" range
from the sampled grid), the transfer function as an explicit colour/opacity
lookup, the samples per voxel and a voxel budget; the response carries the
viewport-sized premultiplied image, the range used, and the sampling metrics.
The server bounds every field before allocating -- output size against the
negotiated frame, the voxel budget against its own `--max-volume-voxels`
cap -- and the client refuses a frame of another size, an unusable or
unrequested range, or metrics the request could not have produced. It never
reuses `ViewDataResponse` to transfer volume field data or arbitrary
geometry.

### 5.7 Errors

Use a stable error-code enum with a human-readable message. Initial codes:

- unsupported protocol;
- invalid request;
- unknown dataset;
- dataset open failure;
- cancelled;
- cache budget exceeded;
- resource limit exceeded;
- operation failure;
- internal server error.

The remote client maps cancellation back to `ReadCancelled` and cache-budget
failure back to `CacheBudgetExceeded`, so the existing UI suppression and
lower-level fallback behavior remain intact. Other failures become typed
connection, protocol, or remote-operation exceptions with the server message.

The server will not send stack traces. It may send path and parser context
that the same authenticated operating-system user would see locally.

### 5.8 Compression decision

Protocol 1.0 will not add application-level compression.

Reasons:

- view-bounded typed vectors preserve simple verification and make the
  uncompressed worst case a function of the viewport rather than dataset
  resolution;
- SSH already offers optional stream compression;
- adding zstd would add a dependency, a second size domain, decompression
  limits, and another full-buffer allocation before measurements establish a
  benefit;
- request-specific viewport and page caps keep response arrays below the
  negotiated frame limit.

The implementation will record encoded bytes and request latency in
diagnostics. If representative remote workloads show that transfer dominates,
an additive minor version can introduce a negotiated compressed-view payload
without changing request semantics.

This decision is explicitly part of the review for this plan.

## 6. Client design

`WireConnection` will own:

- one `Channel` (a TCP socket, or the descriptor pair of the ssh stdio
  stream);
- a monotonically increasing atomic request-ID source;
- a send mutex;
- a dedicated receive thread;
- a mutex-protected pending-request map;
- negotiated protocol state;
- connection state and the terminal disconnect reason.

Sending a request will:

1. register its expected response before writing;
2. serialize a verified-size message;
3. write one complete frame under the send mutex;
4. wait on that request's future while polling its `StopToken`;
5. send one cancellation request if local cancellation is observed;
6. decode the typed terminal response.

The receive thread will be the only socket reader. It will verify each frame,
look up the request ID, validate the expected payload type, and complete the
matching promise. This permits the existing Qt worker tasks to issue slices,
line-view queries, Dataset-window page reads, and sequence prefetch
concurrently.

On EOF, socket error, or protocol failure, the client will atomically mark the
connection closed and fail every pending operation. Socket shutdown will be
used to unblock the receiver during application exit.

Reconnect is explicit:

- no in-flight operation is retried;
- no dataset handle survives;
- the UI retains the endpoint and dataset path;
- the user can reconnect and reopen that path;
- a future enhancement may add an opt-in automatic reopen policy, but it is
  not safe to infer idempotence for every future request type.

`RemoteDatasetSession` will maintain the latest cache snapshot from responses,
so ordinary UI diagnostics do not need a second network request after every
query.

## 7. Server design

The `amrexplorer-server` executable will be Qt-free and will link the production
remote, data, query, I/O, cache, and core libraries.

Startup interface:

```text
amrexplorer-server [--stdio | --port PORT] [--threads COUNT]
               [--max-frame-mib SIZE] [--max-datasets COUNT]
```

In `--stdio` mode (the deployment mode, amended 2026-08-16) the server serves
exactly one session over its own stdin/stdout and exits when the stream ends.
The wire moves to private duplicates of the original descriptors, stdout is
redirected to stderr so stray output cannot corrupt a frame, and the first
bytes on the wire are one machine-readable ready line the client scans for
past any login-shell noise:

```text
AMREXPLORER-STDIO 1 TOKEN <token>
```

In `--port` mode the server binds `127.0.0.1` and serves every accepted
connection; port zero remains available for tests and automation, and the
startup line is `LISTENING 127.0.0.1 <port> TOKEN <token>` on stdout. In both
modes the server generates a random 128-bit session token (from the OS CSPRNG
via `std::random_device`) that every client must echo in its handshake; the
check is mandatory and cannot be disabled. Over stdio the token authenticates
nothing beyond the already-authenticated ssh channel; it stays so that the
handshake, and the loopback mode's security story, are one code path.

The server process will have:

- one loopback listener (port mode) or one pre-connected channel (stdio);
- an accept loop supporting multiple client connections (port mode);
- a bounded shared worker pool;
- configured frame, dataset, cache, and outstanding-request limits;
- signal-driven orderly shutdown.

Each accepted connection gets an isolated `ServerSession` containing:

- handshake state;
- a dataset-handle registry;
- active request IDs and their `StopSource` objects;
- a reader/dispatcher;
- a serialized response writer;
- a session stop source.

Control messages are handled promptly by the reader/dispatcher. Potentially
blocking dataset opens and queries run on the worker pool. Workers hold
`shared_ptr` references to datasets, so closing a handle prevents new work
without invalidating an operation that is already unwinding.

Dataset close will:

- remove the handle from the registry;
- request cancellation of its active operations;
- acknowledge the close;
- release the dataset after outstanding worker references finish.

Connection close will request cancellation for all work, close all handles,
and release the session. A slow or non-cooperative read may finish after the
socket is gone, but its response is discarded and it holds no process-global
session state.

The server validates viewport dimensions, visible bounds, field/component
counts, halo limits, page limits, and the computed worst-case response size
before dispatch. It then plans storage reads, performs any whole-block
overfetch internally, and clips the result before passing it to the codec.
The codec accepts only bounded view-result types, so an internal `FabBlock` or
whole-level extract cannot accidentally cross the wire boundary.

The current query and block-read cancellation checkpoints will be reused.
Cancellation latency is therefore bounded by the longest uncancellable
filesystem operation, not by network handling.

## 8. Transport portability

The frame layer reads and writes an abstract `Channel` (amended 2026-08-16):

- `Socket : Channel` -- POSIX sockets on Linux and macOS, Winsock
  initialization, close, shutdown, and error mapping on Windows; explicit
  loopback bind for the server; RAII ownership; a shutdown operation that
  interrupts blocked reads.
- `DescriptorChannel : Channel` (POSIX only) -- a read/write descriptor pair
  such as a process's stdin/stdout. The two descriptors may share one open
  socket (Linux sshd gives a no-pty command exactly that), so its shutdown
  half-closes rather than closing one descriptor. Both directions poll before
  every transfer, keeping deadlines and stop tokens authoritative on pipes
  and sockets alike.
- Broken-pipe termination is suppressed per transfer for sockets
  (`MSG_NOSIGNAL`/`SO_NOSIGPIPE`); a process using `DescriptorChannel`
  ignores `SIGPIPE` so a departed peer surfaces as `EPIPE`.

No networking types will appear in the dataset or Qt-facing APIs.

## 9. UI and command-line workflow

### 9.1 The ssh session (amended 2026-08-16)

The client starts the server itself; the user supplies only an ssh
destination:

```bash
amrexplorer --ssh user@remote /remote/path/to/plt00010
amrexplorer --ssh user@remote --server "~/bin/amrexplorer-server" \
    /remote/path/to/plt00010
```

Under the hood: `ssh -T <keepalives> -- DEST 'exec amrexplorer-server --stdio
--threads 8'`, with the ssh child's stdin/stdout attached to a socket pair the
client holds. The client discards everything up to the ready line, takes the
token from it, and completes the protocol handshake over the same stream.
Authentication prompts (password, keyboard-interactive MFA, host-key
confirmation) are routed through the client binary via `SSH_ASKPASS`. Closing
the session closes the stream; the server reads end-of-stream and exits, so
no process outlives the client on the remote machine.

The loopback listener workflow (`amrexplorer-server --port` plus an SSH
`-L` tunnel) is no longer a client workflow; the `--port` mode serves the
integration tests, the Qt smoke harness, and `amrexplorer-render-equivalence`.

### 9.2 Desktop actions

- **File > Open Remote Plotfile...** / **File > Open Remote Plotfile
  Sequence...** -- one dialog each: ssh destination, server executable, and
  server-visible path(s). Unchanged connection fields reuse the live session;
  a changed destination starts a new one. **Browse...** starts or reuses the
  session and then opens a directory browser over it (protocol 1.1
  `ListDirectoryRequest`: subdirectories only, plotfiles marked, at most 4096
  entries, path resolution identical to dataset opens). A sequence picked
  there plays in name order. Against a 1.0 server the browser reports that
  browsing is unsupported; typed paths still work.
- a session-status entry in diagnostics.

Switching back to a local open remains supported without restarting the
application. Starting a new session replaces the old connection; datasets
open on it fail on their next request rather than being silently migrated.

### 9.3 CLI

Preserve all current local and smoke-test forms. The remote form is:

```text
amrexplorer --ssh SSH_DESTINATION [--server PATH] [REMOTE_PATH ...]
```

One path opens a dataset, multiple paths open a sequence, and no paths only
establishes the session. The token never appears on a command line: the
server generates it and the client reads it from the session's own stream.
Invalid destinations or conflicting options produce a usage error before the
Qt event loop starts.

## 10. Build and source layout

Proposed production layout:

```text
schemas/amrexplorer_wire.fbs
include/amrexplorer/data/DatasetSession.hpp
include/amrexplorer/data/ViewData.hpp
include/amrexplorer/remote/Connection.hpp
include/amrexplorer/remote/RemoteDatasetSession.hpp
src/data/LocalDatasetSession.cpp
src/data/ViewData.cpp
src/data/DatasetPage.cpp
src/remote/Codec.cpp
src/remote/Frame.cpp
src/remote/Connection.cpp
src/remote/RemoteDatasetSession.cpp
src/remote/Server.cpp
tools/amrexplorer_server/main.cpp
```

Exact private-header splitting may change during implementation, but generated
FlatBuffers declarations will remain private to `Amrvis::remote`.

Build policy:

- replace `AMREXPLORER_ENABLE_WIRE_PROTOTYPE` with
  `AMREXPLORER_ENABLE_REMOTE`;
- build remote support by default for normal Qt builds and explicitly for the
  headless server preset;
- generate C++ bindings into the build tree from the checked-in schema;
- do not check generated FlatBuffers code into the repository;
- use an installed compatible FlatBuffers package when available and retain a
  pinned FetchContent fallback;
- make the generated header an explicit dependency of every codec target;
- install `amrexplorer-server` alongside the desktop executable;
- replace the `wire-prototype` preset with a `remote` headless preset that
  builds the server and all remote tests.

The implementation will retain the currently pinned FlatBuffers version until
the production build is green on the compiler matrix, then document the
minimum compatible version separately from the FetchContent fallback version.

## 11. Validation strategy

### 11.1 Codec and framing unit tests

- Round-trip every request, response, enum, optional field, and metadata type.
- Verify domain-to-wire-to-domain equality.
- Prove that encoded view responses contain no cells outside the planned
  viewport plus the explicitly permitted interpolation halo.
- Reject viewport, page, overlay, field/component, and computed-response sizes
  that exceed negotiated limits before allocating or reading data.
- Keep wire codecs unable to accept `FabBlock`, whole-level extracts, or
  volume data types.
- Reject wrong identifiers, truncated buffers, corrupt offsets, invalid
  unions, zero/oversized frames, invalid vector lengths, overflowed sizes, and
  semantically invalid requests.
- Test fragmented reads/writes and connection closure between length and body.
- Test protocol major version rejection and minor version negotiation.
- Test typed error mapping.

### 11.2 Dataset-session equivalence tests

Materialize existing 2-D and 3-D fixtures and compare local with remote:

- catalog metadata and file-version data;
- view-bounded slice cells, masks, coverage, geometry, and metrics;
- pixel-bounded line representations and metrics;
- visible Dataset-window pages;
- cache clear and cache-budget fallback behavior.

Exercise zoomed, panned, coarse/fine-boundary, nonuniform, and maximum-halo
views. For deterministic fixtures, values, masks, source levels, positions,
bounds, and relevant metrics should match exactly. Instrument the server and
assert that serialized cell and geometry extents are a subset of the planned
view bound even when internal reads touched larger FABs.

### 11.3 Server lifecycle tests

- Multiple requests outstanding on one connection.
- Responses correctly matched when completion order differs.
- Concurrent slices against one dataset.
- Multiple datasets on one connection.
- Multiple client connections.
- Cancellation before dispatch, during a query, and racing with completion.
- Dataset close with active work.
- Clean client EOF and abrupt disconnect.
- Server shutdown with connected and idle clients.
- Duplicate request ID and request-before-handshake rejection.
- Resource-limit enforcement without process termination.
- Malicious or extreme viewport requests rejected before expensive reads.

### 11.4 Reconnect tests

- Disconnect fails every pending operation once.
- Old dataset handles cannot be used after reconnect.
- A new connection can reopen the same path and resume queries.
- Destroying a connection unblocks its receiver and does not delay process
  shutdown.

### 11.5 Qt smoke tests

Extend the current materialized-fixture smoke harness to:

- launch an ephemeral loopback server;
- open a remote 2-D dataset and wait for the initial slice;
- open a remote 3-D dataset and verify all three initial views;
- create a remote line plot;
- populate the remote Dataset window;
- step a two-frame remote sequence;
- cancel or supersede an in-flight remote slice;
- shut down without a hanging Qt thread pool or receive thread.

The test driver must always terminate the child server and print both process
logs on failure.

### 11.6 Build matrix and manual validation

- Current default, debug, headless, sanitizer, and new remote presets.
- GCC, Clang, AppleClang, and MSVC warning-as-error builds.
- ASan/UBSan server and client integration tests.
- Manual SSH-tunnel run against a representative remote plotfile.
- Diagnostics review at native and maximum slice sizes.
- Wire-size review across zoom levels proving that payload size follows the
  viewport and requested halo, not the underlying FAB or dataset extent.
- A protocol inspection confirming that no full FAB, unrestricted level,
  native-resolution line, volume data, or volume geometry is serialized.

## 12. Implementation sequence

### Phase 1: Local abstraction with no networking

1. Move Dataset-window extraction out of Qt and make it page-bounded.
2. Add `DatasetSession` and `LocalDatasetSession`.
3. Add the shared view planner and bounded owning view-result types.
4. Route all current Qt data access through the session.
5. Remove duplicate metadata reads during local open.
6. Run the complete existing test and smoke suite.

Exit criterion: local behavior and all existing features remain unchanged
without FlatBuffers or a server.

### Phase 2: Production schema, codecs, and framing

1. Add the viewport-bounded schema and build-time generation.
2. Implement catalog, view, page, and cache conversion with semantic
   validation.
3. Make the wire codec accept only bounded view-result types.
4. Promote and port the framing/socket layer.
5. Add protocol, codec, corruption, overfetch, and limit tests.

Exit criterion: every operation round-trips through verified buffers and the
transport tests pass on supported platforms.

### Phase 3: Concurrent headless server

1. Add handshake and session state.
2. Add dataset registry and open/close.
3. Dispatch view-data and Dataset-page requests through the shared planner.
4. Enforce viewport, halo, page, geometry, and response-size limits before
   serialization.
5. Add request tracking, cancellation, bounded workers, and shutdown.
6. Add server lifecycle, no-wire-overfetch, and resource-limit tests.

Exit criterion: a non-Qt integration client exercises all operations,
concurrency, cancellation, and teardown.

### Phase 4: Production client and remote dataset session

1. Add the single-reader connection and pending-request map.
2. Add request futures, cancellation forwarding, and typed errors.
3. Add `RemoteDatasetSession` and cache snapshots.
4. Add disconnect and reconnect/reopen tests.

Exit criterion: local and remote dataset-session equivalence tests pass.

### Phase 5: Qt and CLI integration

1. Add dataset-location routing and remote connection ownership.
2. Add remote CLI parsing and desktop dialogs/actions.
3. Enable remote single datasets, sequences, prefetch, line plots, and the
   Dataset window.
4. Add connection diagnostics and user-facing disconnect handling.
5. Add the remote Qt smoke suite.

Exit criterion: the local UI has feature parity for the named remote
workflows, including cancellation and clean exit.

### Phase 6: Packaging, documentation, and prototype retirement

1. Install the server and production remote library dependencies.
2. Update `README.md`, `INSTALL.md`, `docs/building.md`, and the user guide.
3. Document the SSH-tunnel workflow, limitations, and troubleshooting.
4. Replace the prototype preset.
5. Delete `prototypes/flatbuffers_wire` only after its demo is covered by
   production tests.
6. Run the full build/test matrix and the manual SSH validation.

Exit criterion: there is one documented production path and no duplicate
prototype implementation.

## 13. Acceptance criteria

The architecture is complete when:

- existing local-file behavior and tests still pass;
- the Qt client can open the same supported dataset types remotely;
- slice, contour, vector, line-plot, Dataset-window, sequence, animation, and
  export workflows operate with server-side data access;
- one connection safely supports overlapping UI and prefetch requests;
- superseded work is cancelled on the server;
- disconnects fail promptly and cleanly, and reconnect/reopen succeeds;
- all received FlatBuffers are bounded and verified before access;
- every 1-D/2-D response is bounded by the requested viewport plus its
  explicit interpolation halo;
- Dataset-window responses contain only the requested visible page;
- full FABs, whole levels, native-resolution lines, volume data, and volume
  geometry never cross the wire;
- server resource limits prevent unbounded client-controlled allocation;
- the server serves the deployment session over ssh stdio (binding nothing)
  or binds only to loopback for tests, enforces a mandatory per-session
  access token on the handshake, and the SSH security boundary is documented;
- the server and client shut down without blocked receive or worker threads;
- generated bindings come only from the checked-in schema at build time;
- production tests replace the prototype demo, and the prototype is removed.

## 14. Review decisions

Please review these choices before implementation:

1. `DatasetSession` is the shared local/remote boundary; block reads are not a
   public remote operation.
2. The server relies on SSH for transport security. In deployment it speaks
   over the ssh session's stdio and binds nothing (amended 2026-08-16); the
   loopback listener remains for tests, protected by a mandatory per-session
   token so a shared loopback interface does not expose one user's server to
   another local user.
3. Protocol 1.0 transfers only the cells, masks, clipped AMR
   coverage/geometry, requested components, and interpolation halo needed to
   rasterize the current 1-D/2-D view.
4. Remote paths are entered explicitly; there is no remote file browser.
5. Reconnect is explicit -- a new ssh session -- and reopens datasets;
   requests are never automatically replayed.
6. Remote support is built by default for the Qt application, with generated
   code kept out of git.
7. The first production release aims for the listed feature parity rather
   than limiting remote support to slices alone.
8. Protocol 1.0 has no application compression or chunking; its hard payload
   bound comes from viewport/page limits and the negotiated frame limit.
9. 3-D volume rendering (protocol 1.2) returns server-rendered frames and
   never transfers volume field data or arbitrary volume geometry.

Implementation should begin only after these decisions and any requested
scope changes are approved.
