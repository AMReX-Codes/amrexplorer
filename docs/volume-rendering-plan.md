# Volume rendering for 3-D plotfiles — implementation plan

The design and phasing agreed on 2026-08-17 for direct volume rendering, the
companion to `remote-client-server-plan.md`'s "future volume rendering"
decisions. It was implemented as the eight PRs listed under *Phases*; this
document records the reasoning behind their shape.

## Context

AMReXplorer shows 3-D plotfiles as three orthogonal slice panels plus an
orthographic wireframe quadrant (`IsoWidget`). Legacy Amrvis also offered
direct volume rendering (VolPack ray casting with a palette + opacity ramp),
which users still ask for (`dev-notes/todo.org:11,27`). The goal is an
Amrvis-style volume view that also works for remote plotfiles, with no new
dependencies, on CPU, in the existing Qt-free compute layers so it is
headless-testable and server-runnable.

Decisions taken with the user:
- **Technique:** direct volume rendering, orthographic, front-to-back ray casting
  with a transfer function (palette colour + opacity ramp). Isosurfaces later.
- **Remote from day one.** `docs/remote-client-server-plan.md` (lines ~115-118,
  224-230, 411-414, 910-911) already commits to distinct
  `RenderedFrameRequest`/`RenderedFrameResponse` messages: the **server renders
  a viewport-sized frame** and never ships volume data. So the renderer lives
  below `src/qt` and is called by `LocalDatasetSession` on both ends.
- **No new deps, CPU:** multi-threaded software ray caster.
- **UI:** a separate **Volume window** whose central view reuses `IsoWidget`
  (drag-rotate, wheel-zoom, XY/XZ/YZ presets, domain wireframe, grid boxes,
  slice-plane quads); the volume image is drawn *behind* those overlays with the
  identical camera. The main window's quadrant stays wireframe-only.
- **Opacity:** simple ramp (low/high thresholds, max opacity) plus "use palette
  alpha"; `Palette` gains the legacy `.pal` alpha ramp (parsed-and-discarded
  today). No curve editor now.
- **Sampling:** composite the AMR hierarchy (finest-available up to the Level
  combo, as slices do) into a **bounded uniform float grid** (voxel budget,
  256^3 default), cache it per (dataset, field, level, region, dims), and
  ray-cast the cached grid so rotation only re-casts.

## Architecture (what goes where)

```
core      + Volume.hpp (request/frame/grid types, validator), OrthoProjection.hpp (camera math shared with IsoWidget)
query     + VolumeQuery (AMR -> uniform grid "paint" sampler)
render3d  NEW lib, core only: VolumeRaycaster (threads, StopToken)
data      DatasetSession::renderVolume/supportsVolumeRendering; LocalDatasetSession sample+cache+render; SessionValidation
pipeline  + VolumePipeline (transfer-function LUT from Palette+ramp, range resolution, frame-budget bound, cache-pressure fallback loop)
remote    protocol 1.2: RenderedFrameRequest/Response, Server bounding+dispatch, Connection, RemoteDatasetSession
qt        VolumeController (Hooks+signals collaborator), VolumeWindow (IsoWidget view + controls dock), View menu action
```
Layering after: `core <- render3d <- data`; `render2d` untouched (core only);
`amrexplorer_data` links `render3d`; `qt` links `render3d`. Add
`add_subdirectory(src/render3d)` between `src/query` and `src/data` in the root
`CMakeLists.txt` (~line 183). Namespace is `amrvis` (Qt: `amrvis::qt`).

Key design points (each keeps something existing honest):
1. **The wire carries the transfer function as an explicit LUT** (`colors[N]`,
   `opacities[N]`, N = `Palette::colorSlots` = 253) built client-side; the
   server never sees palette names/files, and value→slot uses the same
   `trunc(normalized*(N-1))` as `renderScalarPlane`
   (`src/render2d/ScalarRenderer.cpp:96-137`) so volume colours match slices
   and the colour bar.
2. **Sampler = per-block scatter ("paint"), coarse-to-fine**, each block pinned
   only while painted (a whole-domain gather would pin every FAB of every level
   and trip `CacheBudgetExceeded`). Same values as `SliceQuery::valueAt`
   finest-available composition; no 3-axis `BlockGrid` needed.
3. **Camera math moves to `core`** and `IsoWidget` calls it, so the wireframe
   and the ray caster cannot drift.
4. `VolumeGrid` uses **NaN = uncovered** (no parallel `valid` array).
5. `VolumeFrame.pixels` are **premultiplied 0xAARRGGBB, row 0 at the top** →
   `QImage::Format_ARGB32_Premultiplied` directly (no `displayImageFor` flip).
6. `VolumeRenderRequest::range` is **optional**: File/Level/User are resolved
   client-side (via `requestRange`, memoised remotely) and sent; Visible sends
   `nullopt` and the renderer resolves from the grid's finite extrema and reports
   `usedRange`.
7. `DatasetSession::renderVolume` is a **non-pure virtual with a throwing
   default** plus `supportsVolumeRendering()` (default false), like
   `maximumResponseBytes()`; existing test fakes are untouched; the menu action's
   enablement is derived from `supportsVolumeRendering()`.
8. Move `paddedIfDegenerate` from `pipeline/SliceRangeResolver` to `core` (data
   needs it for Visible-range resolution); keep a forwarding declaration.

Resolved defaults (routine calls, stated so nobody re-asks): alpha byte =
legacy percent, `opacity = min(1, byte/100)` (Amrvis `Palette.cpp:670`);
`reversed()` reverses RGB only; voxel budget 256^3 default / 512^3 hard cap;
grid cache 256 MiB per session, server flags `--max-volume-voxels`,
`--volume-cache-mib`; point sampling at voxel centres (box averaging later);
window mirrors the main window's field/level/range/log/palette (no own range
controls); drafts at half the logical size, the settled frame at the view's
device pixels (`volumeOutputSize`).

## Phases (one PR each; sizes S/M/L)

Dependencies: 1 ⟂; 2 → 3, 2 → 4; 1,3,4 → 5; 5 → 6; 5 → 7 (7 renders remotely
once 6 lands, no code change); 7 → 8.

### PR1 — Palette alpha ramp (S)
Files: `include/amrexplorer/render2d/Palette.hpp`, `src/render2d/Palette.cpp`,
`resources/generate_palettes.py`, `palettes/*.pal`, `tests/unit/test_palette.cpp`.
- Storage `std::array<std::uint8_t, slotCount> alpha_{}` + `bool hasAlphaRamp_`.
  API: ctor overload with optional alpha; `bool hasAlphaRamp() const noexcept`;
  `double opacity(int index) const noexcept` (percent semantics, clamped to 1;
  0 without a ramp — the transfer builder owns the default policy).
  `Palette::load`: 1024-byte files keep bytes 768..1023 as the ramp. Update the
  class comment. `operator==` stays defaulted (now includes alpha; nothing in
  `src/` compares palettes with `==`).
- Verified: `palettes/rainbow.pal` carries the legacy 0..100 ramp; the six
  generated palettes carry a flat 255 (→ clamps to fully opaque, useless).
  Change `generate_palettes.py` to write Amrvis's default linear 0..100 ramp and
  regenerate those six (RGB unchanged); builtins (compiled RGB tables) carry no
  ramp.
- Tests: rainbow → `hasAlphaRamp()`, `opacity(3)==0`, `opacity(255)==1`,
  monotone; 768-byte copy → no ramp; `reversed()` keeps alpha; equality with and
  without ramp.

### PR2 — Core types, validator, shared ortho camera; IsoWidget uses it (S/M)
New `include/amrexplorer/core/Volume.hpp` (+ `src/core/Volume.cpp`):
```cpp
struct OrthoCamera { double azimuth = 0.0, elevation = 0.0, zoom = 1.0; };   // radians
struct VolumeTransferFunction { std::vector<std::uint32_t> colors; std::vector<float> opacities; };
struct VolumeRange { double minimum = 0.0, maximum = 1.0; bool logarithmic = false; };
struct VolumeRenderRequest {
    DatasetId dataset; FieldId field; int component = 0;
    int maximumLevel = 0; CompositionPolicy composition = CompositionPolicy::FinestAvailable;
    RealBox region; OrthoCamera camera; std::array<int, 2> outputSize{0, 0};
    std::optional<VolumeRange> range; bool logarithmic = false;      // logarithmic used when range is nullopt
    VolumeTransferFunction transfer; int samplesPerVoxel = 2;
    std::uint64_t maximumVoxels = defaultVolumeVoxelBudget;           // 256^3
};
struct VolumeGrid { std::array<int,3> dims{}; RealBox region; std::vector<float> values; std::uint64_t coveredVoxels = 0; int maximumLevel = 0; };
struct VolumeRenderMetrics { gridDims, coveredVoxels, sampledMaximumLevel, gridFromCache, sample/renderMicroseconds, candidateBlocks, blocksRead, cacheHits, payloadBytesRead };
struct VolumeFrame { int width = 0, height = 0; std::vector<std::uint32_t> pixels; VolumeRange usedRange; VolumeRenderMetrics metrics; int cacheFallbackFromLevel = -1, cacheFallbackToLevel = -1; };
constexpr int maxVolumeOutputDimension = 4096; constexpr std::size_t maxVolumeTransferEntries = 1024;
constexpr int maxVolumeSamplesPerVoxel = 8; constexpr std::uint64_t defaultVolumeVoxelBudget = 256^3, maxVolumeVoxelBudget = 512^3;
std::vector<std::string> validateVolumeRenderRequest(const VolumeRenderRequest&, int datasetDimension);
```
Validator in the style of `validateSliceRequest` (`src/core/Request.cpp:29-55`).

New `include/amrexplorer/core/OrthoProjection.hpp`: exactly `IsoWidget`'s
math (`src/qt/IsoWidget.cpp:122-128, 147-167`) plus depth and rays:
`ViewportFrame viewportFrame(int w, int h, double margin = 12.0)`;
`ProjectedPoint projectPoint(camera, frame, domain, point)` (normalise by max
domain extent about the domain centre; `x1 = nx cosAz − ny sinAz`, `y1 = nx sinAz
+ ny cosAz`, `y2 = y1 cosEl − nz sinEl`, `depth = y1 sinEl + nz cosEl`; `x =
cx + scale·zoom·x1`, `y = cy − scale·zoom·y2`); `Ray pixelRay(camera, frame,
domain, px, py)` (inverse rotation, origin outside the unit box, direction away
from the viewer); preset constants XY `{0,0}`, XZ `{0,−π/2}`, YZ `{−π/2,−π/2}`
(`IsoWidget.cpp:57-65`).

`IsoWidget` refactor (behaviour-preserving): store `OrthoCamera m_camera`, use
`projectPoint`; add `camera()`, `setCamera()`, signals `cameraChanged()` and
`interactionEnded()`.

Tests: `test_ortho_projection` pins current numeric values (a corner under
30°/30°/zoom 1 in 400×300), preset mappings, `pixelRay` round trip within 1e-12;
`test_volume_types` validator accept/reject; existing 3-D smoke tests guard the
quadrant.

### PR3 — VolumeQuery: AMR → uniform grid (M)
`include/amrexplorer/query/VolumeQuery.hpp`, `src/query/VolumeQuery.cpp`:
```cpp
struct VolumeSampleRequest { DatasetId dataset; FieldId field; int component = 0; int maximumLevel = 0;
    CompositionPolicy composition = FinestAvailable; RealBox region; std::uint64_t maximumVoxels = defaultVolumeVoxelBudget; };
struct VolumeQueryResult { VolumeGrid grid; SliceQueryMetrics metrics; };
std::array<int,3> volumeGridDims(const DatasetMetadata&, const RealBox& region, int maximumLevel, std::uint64_t maximumVoxels);
class VolumeQuery { explicit VolumeQuery(PlotfileDataset&); VolumeQueryResult execute(const VolumeSampleRequest&, StopToken = {}); };
```
Algorithm: validate like `SliceQuery::execute` (`src/query/SliceQuery.cpp:95-113`);
`dims = volumeGridDims` (native cells of `maximumLevel` in the region, scaled by
`cbrt(budget/product)` when over budget); fill NaN; for `level = minimumLevel..
maximumLevel` ascending, grids in descending index (smallest index wins, matching
`lookupBlockValue`'s first-match order): skip non-intersecting (`detail::intersects`),
`requestBlock` (metrics as SliceQuery), `requireBlockPayload`, physical box via
`sampleBounds`, per-axis voxel index range whose centres fall in the block, cell
index per voxel via `sampleIndex`, copy `fab.values[valueOffset(...)]` (3-D
branch exists, `BlockLookup.hpp:101-113`); release the handle before the next
block; poll cancellation per block and every 32 z-slabs. Single-threaded v1.
Tests (`tests/integration/test_volume_query.cpp`, synthesised fixtures as
`test_slice_query.cpp:78-122`): 4^3 `(i+j+k)/9` exact at budget ≥ 64, budget 8 →
2^3 centre-cell values, sub-region; a synthesised 2-level fixture (coarse 1.0,
fine block 2.0): FinestAvailable/ExactLevel/maxLevel 0 expectations; cancelled
token → `ReadCancelled`; region outside domain → `invalid_argument`.

### PR4 — render3d ray caster (M)
New lib `src/render3d/CMakeLists.txt` (mirror `src/render2d/CMakeLists.txt`,
`PUBLIC amrexplorer::core`, `PRIVATE Threads::Threads amrexplorer::warnings`).
`include/amrexplorer/render3d/VolumeRaycaster.hpp`:
```cpp
struct RaycastSettings { OrthoCamera camera; RealBox domain /*normalisation box*/; std::array<int,2> outputSize;
    VolumeRange range; VolumeTransferFunction transfer; int samplesPerVoxel = 2; unsigned threadCount = 0; };
VolumeFrame raycastVolume(const VolumeGrid&, const RaycastSettings&, StopToken = {});
std::optional<std::pair<double,double>> volumeGridRange(const VolumeGrid&, bool logarithmic, StopToken = {});
int raycastThreadCount(unsigned requested, int height);
```
Per pixel: the frame's `rayField` stepped per column and row (the camera's
rotation and normalisation are resolved once, not per pixel), slab-intersect
`grid.region` (a negative entry parameter is kept — the region need not be the
domain), `step = referenceLength/samplesPerVoxel` where `referenceLength` is
the reciprocal of `hypot(direction/pitch)`, so the step follows the distance a
ray travels through one voxel's worth of material rather than the smallest
pitch or the voxels it enters; the march is an integer sample count in voxel
coordinates, not an accumulation. Nearest-voxel sample, NaN skipped, value →
slot via `core/ValueMapping.hpp`, which `renderScalarPlane` shares (log: `v<=0`
skipped), opacity corrected per step `1-(1-a)^(1/samplesPerVoxel)`,
front-to-back premultiplied compositing, early exit at `A ≥ 0.999`. Threads:
rows in contiguous bands (`std::thread`, bounded by `raycastThreadCount`),
token polled every 64 columns *and* every 4096 samples via an atomic flag —
per-row polling leaves a single row, and per-pixel polling a single ray,
uninterruptible — join then throw `ReadCancelled`. Bit-identical for any
thread count, within one build.
Tests (`tests/unit/test_volume_raycaster.cpp`): single opaque voxel lands in its
projected footprint (`projectPoint` on the 8 corners), nothing else lit; uniform
slab alpha `1-(1-a)^N` independent of samplesPerVoxel; preset gradients along
expected screen axes; thread-count invariance; log slot mapping; cancellation;
`volumeGridRange` NaN-only → nullopt. Benchmark `tests/benchmark/bench_volume_render.cpp`
registered like `bench_slice_query` (functional guard, generous timeout).

### PR5 — Session API, local implementation, VolumePipeline (M)
- `include/amrexplorer/data/DatasetSession.hpp`: `virtual bool
  supportsVolumeRendering() const noexcept { return false; }`, `virtual
  VolumeFrame renderVolume(const VolumeRenderRequest&, StopToken = {})` throwing
  by default.
- `LocalDatasetSession`: supports iff 3-D && !isFab && hasPhysicalGeometry.
  `renderVolume`: `validateSessionVolumeRequest`; grid cache
  (`ByteLruCache<VolumeGridKey, VolumeGrid>` 256 MiB, key {field, component,
  maximumLevel, composition, region, dims}; `findAndPin`/`insertAndPin`; cleared
  by `close()`/`clearUnpinnedCache()`); on miss `VolumeQuery::execute`; range =
  request or `volumeGridRange` with linear fallback (mirror `resolveDisplayRange`,
  `src/pipeline/SliceRangeResolver.hpp:86`); `raycastVolume` with `domain =
  datasetSampleBounds(metadata)`; metrics/timings.
- `SessionValidation` (`src/data/SessionValidation.cpp`, model
  `validateSessionViewResult` at :215): `validateSessionVolumeRequest` (dataset id,
  field/level/component, region inside bounds) and `validateSessionVolumeResult`
  (width/height == outputSize, `pixels.size()==w*h`, finite usedRange, equals the
  request's when given, gridDims ≥1 with product ≤ maximumVoxels,
  sampledMaximumLevel bounds, fallback levels consistent).
- `include/amrexplorer/pipeline/VolumePipeline.hpp`: `struct OpacityRamp
  {lowThreshold, highThreshold, maximumOpacity, usePaletteAlpha}`;
  `makeVolumeTransferFunction(const Palette&, const OpacityRamp&)` (253 entries,
  colours = slot RGB, opacity 0 outside [low,high], linear inside or
  `palette.opacity(slot)` when usePaletteAlpha && hasAlphaRamp, × maximumOpacity);
  `resolveVolumeRange(...)` (File/Level/User explicit via `requestRange`, Visible →
  nullopt); `frameBudgetBoundedVolumeSize(outputSize, maximumResponseBytes)`
  (w*h*4 + overhead, sibling of `frameBudgetBoundedOutputSize`,
  `SlicePipeline.cpp:118-160`); `executeVolumeRenderWithFallback(...)` (the
  `executeSliceWithFallback` cache-pressure loop, `SlicePipeline.cpp:350-400`).
- Tests: `test_volume_pipeline` (LUT shape/values, ramp cases, palette-alpha
  path), extend `test_session_validation`, local end-to-end render over the
  synthesised fixture (coverage > 0, second call `gridFromCache`, render after
  `close()` throws).

### PR6 — Remote protocol 1.2 (L)
`schemas/amrexplorer_wire.fbs`: `RenderedFrameRequest` (dataset_id, field,
component, maximum_level, composition, region, azimuth, elevation, zoom, width,
height, has_range, minimum, maximum, logarithmic, transfer_colors:[uint],
transfer_opacities:[float], samples_per_voxel, maximum_voxels) and
`RenderedFrameResponse` (width, height, pixels:[uint], used_minimum/maximum/
logarithmic, grid_dims:[int], covered_voxels, sampled_maximum_level,
grid_from_cache, sample/render_microseconds, candidate_blocks, blocks_read,
cache_hits, payload_bytes_read, cache_fallback_from/to_level, cache:CacheState);
union tags 27/28. Touchpoints (all verified): `Protocol.hpp` (`protocolMinorVersion
= 2`, `PayloadKind` 27/28); `Codec.cpp:20-51` asserts + `payloadKind()` upper
bound (:193-200); `Codec.{hpp,cpp}` toWire/fromWire (model: slice pair
`Codec.cpp:825-921`; `requireFinite`, `checkedProduct`, `validateResultVectors`);
`Server.cpp` handle switch (:542-591) + `requestDataset` (:1002-1044) + handler
modelled on `sliceView` (:738-772): decode → gate minor < 2 → `UnsupportedProtocol`
→ `validateVolumeBound` (outputSize in [1,4096], `fitsResponse(w*h*4)`, clamp
`maximumVoxels` to `ServerOptions::maximumVolumeVoxels`, LUT ≤ 1024) →
`requireDataset` → `renderVolume` → exact size guard → `send`; `ServerOptions`
+ `tools/amrexplorer_server` flags; `Connection.{hpp,cpp}` `renderVolume` gated
like `listDirectory` (`Connection.cpp:255-260`), `ResponseWait::Indefinite`;
`RemoteDatasetSession` (`supportsVolumeRendering` = negotiated minor ≥ 2 && 3-D;
`renderVolume` inside `refusingInvalidResponses` + result validation);
`tests/fuzz/fuzz_wire_codec.cpp` switch arms (:326-390) + seeds (:454);
`tests/unit/test_remote_codec.cpp` round trips/rejections;
`tests/integration/test_remote_session.cpp` over `remote_server_fixture_3d`: local
vs remote frames byte-identical, oversize frame → `ResourceLimitExceeded`,
mid-render cancel; `test_remote_server.cpp`: a 1.1 client gets
`UnsupportedProtocol`; docs (`remote-client-server-plan.md` "future" passages →
concrete 1.2; `ARCHITECTURE.md` trust boundaries).

### PR7 — GUI: Volume window, VolumeController, IsoWidget backdrop (L)
- `IsoWidget`: `setBackdropImage(QImage)` drawn scaled to `rect()` after the fill
  and before boxes (null image = unchanged quadrant); `setLevelBoxesVisible`,
  `setDomainOutlineVisible`.
- `src/qt/VolumeWindow.{hpp,cpp}` (`QMainWindow`, `WA_DeleteOnClose`, one
  instance; pattern `MainWindow::showDatasetWindow`, `src/qt/MainWindowDataset.cpp:485-515`):
  central `IsoWidget`; right dock controls (opacity low/high sliders, max opacity,
  "Use palette alpha" enabled iff `hasAlphaRamp()`, quality combo Draft/Normal/High
  = samplesPerVoxel 1/2/4 with 128^3/256^3/384^3, Boxes/Slice planes/Domain
  outline toggles, status label, "Rendering…" indicator). Signals `rampChanged`,
  `qualityChanged`, `overlayToggled`, `cameraChanged`, `interactionEnded`.
- `src/qt/VolumeController.{hpp,cpp}` — collaborator per `docs/ARCHITECTURE.md`
  (Hooks: dataset, field, levelSelection, rangeSelection, palette, slicePositions,
  slicePlanesVisible, isShuttingDown; signals: `renderActivityChanged(int)`,
  `renderFailed(QString)`, `statusMessage`, `staleResultDropped`, `frameDisplayed`).
  `createAction()` "&Volume Rendering..." (View menu after Slice Planes; enabled
  derived from `dataset->supportsVolumeRendering()`), `showWindow`, `closeWindow`,
  `configureForDataset`, `refresh` (40 ms debounce), `slicePositionsChanged`,
  `slicePlanesVisibilityChanged` (overlay only), `reset`, `cancel`. Scheduling
  = single-flight + rerun flag + generation + `StopSource`, worker
  `QtConcurrent::run(executeVolumeRenderWithFallback)` + `QFutureWatcher`
  (pattern `src/qt/MainWindowSlice.cpp:226-475`, `syncVisibleRanges`); progressive:
  half-size + samplesPerVoxel 1 while dragging, full on `interactionEnded`;
  request: region = domain, outputSize = `frameBudgetBoundedVolumeSize(viewSize,
  maximumResponseBytes)`, range via `resolveVolumeRange` on the worker, LUT via
  `makeVolumeTransferFunction`, camera from the window; errors →
  `reportBackgroundError` (`ReadCancelled` silent).
- MainWindow wiring: construct next to the other collaborators; menu action after
  `m_slicePlanesAction` (`MainWindow.cpp:1103`); `refresh()` from field/level
  handlers (`MainWindow.cpp:211-233`), `RangeController` signals (:235-243),
  `refreshPaletteDisplay()` (:1287-1295); `slicePositionsChanged()` from
  `setSlicePosition` (`MainWindowSlice.cpp:139-168`); `configureForDataset()` where
  `m_isoWidget->setGeometry(metadata)` runs and on `sequenceFrameDisplayed`;
  `reset()` beside `closeDatasetWindow()` in teardown; `cancel()` in
  `cancelInFlight()`; `src/qt/CMakeLists.txt` sources + `amrexplorer::render3d`.
- Tests: `tests/unit/test_volume_controller.cpp` with a `FakeSession`
  (shape of `test_particle_controller.cpp:38`): action enablement, one request
  sized to the view, two quick refreshes coalesce, drag → half-size then full,
  `reset()` drops a late result, throwing session → `renderFailed`.
  `MainWindowTestAccess.cpp`: `showVolumeWindowForTest`, `volumeFrameAlphaCoverageForTest`,
  `volumeWindowOpenForTest`. Smoke `src/qt/SmokeHarnessVolume.cpp` (dispatch in
  `SmokeHarness.cpp:19-30`; driver modes in `tests/qt_smoke_driver.cmake`):
  `--volume-smoke-test <plotfile_3d>` (coverage > 0 after `frameDisplayed`) and
  `--remote-volume-smoke-test` via `attachSmokeServer` (`SmokeHarnessRemote.cpp:35-47`,
  fixture `remote_server_3d_materialized`).

### PR8 — Polish and docs (S/M)
Volume window "Export Image..." (PNG; `grab()` would bake the preset buttons
in, so the view is rendered without its children); hi-DPI (device-resolution
render — `volumeOutputSize` scales the settled frame by the view's ratio, and
`backdropScale` already divides out both projections' scales, so the margin
term needed no change and the domain's pixels land 1:1); trilinear option
(request field + wire field, quality High); `docs/user-guide.md` new "Volume
rendering" section after "Working with 3-D data" (:269); `docs/ARCHITECTURE.md`
layering diagram (`render3d`), collaborator table row, "Where to start reading"
row, threading note; `docs/building.md` unchanged.

## Cross-cutting

- **Threading:** GUI — one QtConcurrent task per render, caster threads inside,
  joined before return. Server — one pool worker per request (`Server.cpp:76-152`),
  caster threads inside; `ServerOptions::renderThreads` (0 = auto) escape hatch.
- **Cancellation:** `StopToken` per block/z-slab in the sampler, per 16 rows in
  the caster; `ReadCancelled` propagates like slices; server `CancelRequest`
  unchanged.
- **Memory:** 256^3 floats = 64 MiB per grid; 256 MiB grid cache per session; server
  worst case `maximumDatasets` × cache; 512^3 hard cap. Frame ≈ w·h·4 + 1 KiB
  (1000×800 ≈ 3.2 MB; half-res drag frames ≈ 0.8 MB).
- **Determinism:** pixel work independent of thread partition → local == remote
  within one binary (equivalence test); cross-platform bit equality not claimed.
- **Trust boundaries:** server bounds every field before allocating; client
  refuses malformed frames in `validateSessionVolumeResult` and closes the
  connection via `refusingInvalidResponses`.

## Verification (end to end)

- Per PR: `cmake --build build -j` clean under `-Werror`; `ctest --test-dir build -j8`
  (136 tests today) plus the new unit/integration/smoke tests named above;
  mutation-check the new assertions (e.g. break opacity correction → slab test
  fails; drop the paint order → 2-level test fails; skip result validation → a
  crafted oversize response test fails).
- Remote: `test_remote_session` local-vs-remote frame equality over
  `plotfile_3d`; `fuzz_wire_codec` with the new seeds; `test_remote_server`
  UnsupportedProtocol for a 1.1 client; run the `remote`, `qt-sanitizers` and
  `tsan` presets locally (`ctest --preset …`) since the caster adds threads.
- Manual: `./build/src/qt/amrexplorer <3-D plotfile>` → View → Volume Rendering…;
  rotate/zoom/presets, thresholds, palette alpha with `rainbow.pal`, level combo,
  Visible/User ranges, log; the same via `--ssh` against a rebuilt server, and
  against an old (1.1) server the action stays disabled.
- Benchmark: `bench_volume_render` numbers in the PR description (Mpx/s at
  512×512 over 256^3, 2 samples/voxel).

## Follow-ups (deliberately out of the first version)

- **Opacity curve editor** — a draggable control-point opacity curve drawn
  over the palette (Amrvis's palette window), replacing the two-threshold
  window + maximum as the way to shape the transfer function; the request
  already carries an explicit lookup, so this is UI only.
- **Isosurfaces** — extracted and shaded iso-value surfaces alongside the
  translucent volume; needs a marching-cubes pass over the sampled grid and
  a depth-composited draw.
- **Trilinear sampling** — a request/wire field and the High quality preset;
  nearest-voxel today.
- **Box-averaged downsampling** — when the voxel budget forces a coarser
  pitch than the finest level, average the cells under a voxel instead of
  taking the one at its centre.
- **Progressive painting** — paint the grid level by level (or block by
  block) so a huge plotfile shows something before its finest level is read;
  today the sample runs to completion before the first frame.
- **Animation export of the volume view** — the exporter's `FrameRenderer`
  takes an `ImageView*`; the volume window needs its own "render me a QImage"
  seam to join a sequence export.
- **Frame compression on the wire** — RLE or 8-bit indexed pixels for slow
  links; half-resolution drafts are the mitigation today.
- **End-to-end coverage of "Export Image..."** — the rules it applies (the
  `.png` name, and rendering the view without the preset buttons parked over
  it) are unit-tested in `test_widget_image_export`, but nothing drives the menu
  action, so the slot's own wiring — the name rule, the confirmation, the device pixel
  ratio, and that the picture is taken before the dialogs rather than after
  them — is uncovered. A harness that
  triggered the action and answered its modals from a timer was written and
  withdrawn: it failed roughly 3% of `ctest` runs, because
  `application.exit()` called from a handler that a nested modal loop delivered
  quits that loop rather than the run. Collect a verdict, let the stack unwind,
  and exit from a zero-timer at the outer level; give the dock's status labels a
  horizontal size policy of `Ignored` so showing "Rendering…" stops changing the
  view's size mid-export; and set `QT_SCALE_FACTOR=2` on the test, since the
  offscreen platform reports a ratio of 1 and an export that hardcodes 1
  otherwise passes every assertion.

## Risks

`IsoWidget` refactor (pinned numerically + existing smoke); server CPU/memory
under many rotating clients (single-flight, half-res drag, caps); `-Werror` on
MSVC for the numeric code and `std::thread` in a static lib (tsan/sanitizer CI);
smoke tests assert coverage, never pixels; nodal/face-centred fields leave thin
gaps like slices; large plotfiles sample every block once per grid build (status
"Sampling…", level combo mitigates; level-by-level progressive paint is a
follow-up); protocol bump shifts version-relative tests (parametrised on
`protocolMinorVersion`, still valid).
