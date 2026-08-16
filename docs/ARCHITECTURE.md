# AMReXplorer architecture

A map for contributors: how the code is layered, where the trust boundaries
are, and how work moves between threads. For build instructions see
[building.md](building.md); for user-facing behavior see
[user-guide.md](user-guide.md).

## Layering

Dependencies point downward. Everything below `src/qt` is free of Qt, so the
whole compute path can run — and be tested — headless.

```
              src/qt          GUI: MainWindow*, ImageView, docks, dialogs, main.cpp
                 |            (orchestrates; owns no data-reading logic)
   +-------------+-------------+
   |             |             |
 pipeline      render2d       data          pipeline: SlicePipeline, DisplayCoordinator,
   |             |             |                       SliceRangeResolver, ParticleProjection
 query           |          (LocalDatasetSession,     render2d: ScalarRenderer, Contours,
   |             |           RemoteDatasetSession,               VectorGlyphs, SphericalWarp, Palette
  io            core         SessionValidation, ...)   query:   SliceQuery, LineQuery
   |             |             |             |          io:      plotfile readers, FitsWriter
  core         cache         core          remote      core:    Geometry, Metadata, Request, Result
                                                        cache:   ByteLruCache
                                                        remote:  Frame/Channel, Codec, Connection, Server
```

The guiding split: **the GUI orchestrates, the pipeline computes.** `MainWindow`
turns user actions into requests and marshals results onto the screen; it does no
file or wire I/O itself. `SlicePipeline` (and the query/render layers under it)
turns a `SliceRequest` into a displayable `SliceDisplayResult` — raster image,
contour polylines, vector glyphs, resolved color range — with no Qt dependency.

## The dataset session abstraction

`DatasetSession` (in `src/data`) is the seam the pipeline talks to. Two
implementations, interchangeable to everything above them:

- **`LocalDatasetSession`** — reads plotfiles in-process via `src/io` and the
  `SliceQuery`/`LineQuery` layer, backed by the block cache.
- **`RemoteDatasetSession`** — forwards each request over the wire (`src/remote`)
  to a server that itself runs a `LocalDatasetSession`.

The GUI opens one or the other and is otherwise agnostic to where the data lives.

## Threading model

- The **Qt GUI thread** owns all widgets and view state and never blocks on I/O.
- Each slice/line/metadata/particle request runs on a **QtConcurrent worker**
  (the shared global thread pool); the result returns through a
  `QFutureWatcher::finished` slot on the GUI thread.
- In-flight work is made obsolete two ways that every completion handler checks:
  a monotonic **generation counter** (bumped on dataset open, frame switch, etc.)
  and a cooperative **`StopSource`/`StopToken`** the workers poll (readers check
  their token at chunk boundaries). A stale or cancelled result is dropped, never
  displayed.
- The block **cache** (`ByteLruCache`) is byte-budgeted; a query pins every block
  it composites for the duration of the query, and an over-budget composite
  falls back to a coarser level rather than failing.

## Trust boundaries

Two inputs are untrusted, and each has a dedicated hardening layer. A change near
either should preserve its invariants.

1. **Crafted plotfiles** — `src/io` (esp. `PlotfileMetadataReader`,
   `PlotfileBlockReader`, `ParticleReader`, and `detail/FabHeaderParsing`) treats
   every header field as hostile: bounded token/line/integer reads, reserve sizes
   bounded by real file evidence (not a claimed count), path-containment checks on
   any metadata-derived path, and every geometry offset overflow-checked.
   `validateMetadata` (`src/core`) is the final gate every reader runs.
2. **A malicious remote peer** — `src/remote/Codec` verifies the FlatBuffers wire
   and re-runs `validateMetadata` on any received catalog; `Server` bounds every
   request before allocating and authenticates with a per-session token; and
   `SessionValidation` (`src/data`) is the response-trust boundary — it
   re-derives and cross-checks every server *response* (raster size, region,
   source levels, grid-box provenance, page/particle shape) against the request
   before the client trusts it.

## Where to start reading

| To understand… | Start at |
|---|---|
| A slice request end to end | `src/pipeline/SlicePipeline.cpp` → `src/query/SliceQuery.cpp` |
| Plotfile reading / hardening | `src/io/plotfile/`, `include/amrexplorer/io/detail/FabHeaderParsing.hpp` |
| The remote protocol | `src/remote/Codec.hpp`, `Frame.cpp` (Channel/Socket), `Server.cpp`, `Connection.cpp` |
| Response validation | `src/data/SessionValidation.cpp` |
| The GUI ↔ worker handoff | `src/qt/MainWindowSlice.cpp` (request/arrival), `SequenceController` |
