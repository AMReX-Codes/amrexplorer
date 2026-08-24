# AMReXplorer architecture

A map for contributors: how the code is layered, where the trust boundaries
are, and how work moves between threads. For build instructions see
[building.md](building.md); for user-facing behavior see
[user-guide.md](user-guide.md).

## Layering

Dependencies point downward. Everything below `src/qt` is free of Qt, so the
whole compute path can run — and be tested — headless.

```
              src/qt          GUI: MainWindow* + its controllers, ImageView, docks, dialogs, main.cpp
                 |            (orchestrates; owns no data-reading logic)
   +-------------+-------------+
   |             |             |
 pipeline      render2d       data          pipeline: SlicePipeline, VolumePipeline, DisplayCoordinator,
   |             |             |                       SliceRangeResolver, ParticleProjection
 query           |          (LocalDatasetSession,     render2d: ScalarRenderer, Contours,
   |             |           RemoteDatasetSession,               VectorGlyphs, SphericalWarp, Palette
  io            core         SessionValidation, ...)   render3d: VolumeRaycaster (data links it:
   |             |             |             |                    a session samples and renders)
  core         cache         core          remote      query:   SliceQuery, LineQuery, VolumeQuery
   |                          |                         io:      plotfile readers, FitsWriter
expression               expression                     core:    Geometry, Metadata, Request, Result,
                                                                 Volume, OrthoProjection, DerivedField
                                                        cache:   ByteLruCache
                                                        remote:  Frame/Channel, Codec, Connection, Server
                                                        expression: the algebraic expressions behind
                                                                 derived fields; depends on nothing
```

The guiding split: **the GUI orchestrates, the pipeline computes.** `MainWindow`
turns user actions into requests and marshals results onto the screen; it does no
file or wire I/O itself. `SlicePipeline` (and the query/render layers under it)
turns a `SliceRequest` into a displayable `SliceDisplayResult` — raster image,
contour polylines, vector glyphs, resolved color range — with no Qt dependency.

## The GUI layer

`MainWindow` is the one window; it owns the plane views (`PlaneViewState`
per 2-D view and 3-D panel), the slice request/arrival paths, the
visible-range sync that keeps three 3-D panels on one color range, zoom/pan,
crosshairs and the probe, and the menus and docks. Everything else the window
does is delegated to an **owned collaborator**, each a `QObject` created by
the window, wired to it in one of two ways:

- **`Hooks`** — a struct of `std::function`s the window fills in at
  construction, for what the collaborator must *ask* the window (the open
  dataset, the current frame spec, whether the window is closing, the
  settings store). Hooks are how a collaborator stays testable: a unit test
  supplies fakes. Widget-only collaborators (`PaletteController`,
  `RangeController`) need none; the window reads their state through
  accessors instead.
- **Signals** — for what the collaborator *tells* the window (open this path,
  redraw the overlays, show this status, this failed). The window connects
  them once, next to the construction.

| Collaborator | Owns | Test |
|---|---|---|
| `SequenceController` | the plotfile sequence: frame list, prefetch, frame switches and their generations | `test_sequence_controller` |
| `AnimationExporter` | the animation-export state machine (frames → images/MP4) | via the export smoke tests |
| `PaletteController` | palette choice, reversal, custom `.pal` files, the Palette menu and selector, persistence | `test_palette_controller` |
| `ParticleController` | particle species/fraction/seed/colours, the sample load, the Particles dialog, action and progress bar | `test_particle_controller` |
| `DiagnosticsModel` | the Diagnostics dock: request/stale counters, read and cache metrics, probe and error histories | `test_diagnostics_model` |
| `FabNavigator` | standalone FAB / MultiFab navigation: the selector dock, drill-down and return, the async header reads | `test_fab_navigator` |
| `RemoteSessionController` | the ssh-launched server session and its connection, the Open Remote dialog (`RemoteOpenDialog`), the remote browser (`RemoteFileDialog`), per-destination settings | `test_remote_session_controller`, `test_remote_open_dialog`, `test_remote_file_dialog` |
| `RangeController` | the range mode / User min-max / Log widgets and the per-field range memory | `test_range_controller` |
| `DerivedFieldController` | the derived-field definitions, the Variable menu's Expression Editor and its dialog, the expression-list JSON, and the settings persistence; validates a list before asking the host to reopen the dataset with it | `test_derived_field_controller`, the `--derived-field-smoke-test` harness |
| `VolumeController` | the Volume Rendering window (an `IsoWidget` view with the rendered volume under its wireframe, and the opacity/quality controls), following the field, level, range and palette; render scheduling with drafts while the camera moves | `test_volume_controller` |

Two rules keep the seams honest. A collaborator exposes only what has a
production caller — a public method or signal that exists for a test is a
smell. And anything the window can also toggle (an action's enablement, a
suspended state) is *derived* inside the collaborator from its inputs, never
set from two places; the `ParticleController` action (dataset has species ×
no load running × not suspended by the host) is the reference.

The window's own test surface is the `…ForTest` accessors in
`MainWindowTestAccess.cpp` (compiled only with `AMREXPLORER_QT_TEST_ACCESS`),
driven by the offscreen `--*-smoke-test` harnesses in `SmokeHarness*.cpp` (one
file per theme, dispatched from `main.cpp`); they cover the integration the
collaborators' unit tests cannot -- a slice arriving in a view, a sequence
frame switching, the sync settling.

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
- A **volume render** is one worker task (a `QtConcurrent` run in the GUI, a
  pool worker in the server) inside which the ray caster splits the rows
  across its own short-lived `std::thread`s and joins them before returning;
  the picture does not depend on the split. The sampled grid is cached in the
  session when it fits that cache's budget, so a camera change re-casts
  without re-reading the plotfile; one that does not fit is rendered from the
  local copy and forgotten, and the next camera change resamples it.

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
   source levels, grid-box provenance, page/particle shape, a rendered
   volume frame's size, range and sampling metrics) against the request
   before the client trusts it.

## Where to start reading

| To understand… | Start at |
|---|---|
| A slice request end to end | `src/pipeline/SlicePipeline.cpp` → `src/query/SliceQuery.cpp` |
| A volume frame end to end | `src/pipeline/VolumePipeline.cpp` → `src/data/LocalDatasetSession.cpp` (`renderVolume`) → `src/query/VolumeQuery.cpp` → `src/render3d/VolumeRaycaster.cpp`; the camera math both the view and the caster use is `include/amrexplorer/core/OrthoProjection.hpp` |
| Plotfile reading / hardening | `src/io/plotfile/`, `include/amrexplorer/io/detail/FabHeaderParsing.hpp` |
| A derived field end to end | `include/amrexplorer/core/DerivedField.hpp` (resolve a definition against a dataset) → `src/io/plotfile/PlotfileDataset.cpp` (`readDerivedBlock`) → `src/expression/Expression.cpp`. Installed when the dataset is opened, so above `PlotfileDataset` a derived field is an ordinary `FieldId` |
| The remote protocol | `src/remote/Codec.hpp`, `Frame.cpp` (Channel/Socket), `Server.cpp`, `Connection.cpp` |
| Response validation | `src/data/SessionValidation.cpp` |
| The GUI ↔ worker handoff | `src/qt/MainWindowSlice.cpp` (request/arrival), `SequenceController` |
| Adding a GUI feature | the collaborator table above; `RangeController` is the smallest widget-only example, `DiagnosticsModel` the smallest with `Hooks`, each with its unit test |
