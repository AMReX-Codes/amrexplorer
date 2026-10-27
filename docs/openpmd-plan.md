# Reading openPMD output — implementation plan

A proposed design, drafted on 2026-10-26, for opening openPMD series
(HDF5 or ADIOS2) alongside AMReX plotfiles. The *Phases* section is the
suggested order of PRs; all three phases are done.

## Recommendation

Link **openPMD-api** as an optional dependency and add an `OpenPmdSource`
behind a block-source interface split out of `PlotfileDataset`. Everything
above `src/io` then sees openPMD data as ordinary AMR blocks, exactly as it
sees plotfiles today.

Hand-writing an HDF5 reader was considered and rejected: it would not cover
ADIOS2 (BP4/BP5), which most large WarpX/ImpactX runs write.

## Why this fits the codebase

The layering already does most of the work. Everything above `src/io` speaks
format-neutral types — `DatasetMetadata` / `LevelMetadata` / `BlockMetadata`,
`FabBlock`, `BlockRequest` — and the remote protocol never sends the
plotfile-specific fields (`filePath`, `fileOffset`, `visMfHeaderVersion`).

There is one real coupling: `SliceQuery`, `LineQuery`, `VolumeQuery`,
`MappedGridQuery` and `query/detail/BlockLookup` all take a concrete
`PlotfileDataset&`.

`PlotfileDataset` currently does two jobs:

1. **Format-neutral work:** the byte-budgeted block cache, the I/O mutex and
   its cancellable `try_lock` loop, derived-field installation and
   `readDerivedBlock`, and `requestBlock`'s check-then-read logic.
2. **Plotfile work:** metadata reading, `PlotfileBlockReader`, particle
   discovery, and the `Nu_nd` mapped grid.

## Design

### Split `PlotfileDataset`

- Move job 1 into a generic `AmrDataset`.
- Put job 2 behind a small `BlockSource` interface: `metadata()`,
  `readBlock()`, `fileVersion()`, `particleSpecies()`, `readParticles()`, and
  an optional `mappedGrid()`.
- `PlotfileSource` implements it with today's code; `OpenPmdSource` is new.
- The queries and `LocalDatasetSession` take `AmrDataset&` instead of
  `PlotfileDataset&`. Derived fields, caching and cancellation then work for
  openPMD without extra code.

### Build

- `option(AMREXPLORER_ENABLE_OPENPMD ... OFF)` with `find_package(openPMD)`.
- Source in `src/io/openpmd/`.
- Off by default so CI and the statically linked server build
  (`AMREXPLORER_SERVER_STATIC_CXX_RUNTIME`) are unaffected.

### Choosing a reader

Decide in the code that opens a dataset, from what is on disk: a directory
containing `Header` is a plotfile; `.h5`, `.bp`, or an `%T` file pattern is
an openPMD series.

## How openPMD maps onto this code

- **Levels.** openPMD has no built-in AMR hierarchy. WarpX's convention is a
  separate mesh per level with a `_lvl1`, `_lvl2`, … suffix, each sized to the
  full domain at that level's resolution and only partly filled in.
  `Mesh::availableChunks()` returns the regions actually written, which
  correspond to AMReX boxes, so it can fill `LevelMetadata::boxes`. The
  refinement ratio comes from the ratio of `gridSpacing` between levels.
- **Axis order.** openPMD files usually list axes in C order (`z,y,x`) and
  record a `dataOrder`. Each chunk has to be transposed into the Fortran order
  a `FabBlock` assumes.
- **Centering.** A record component's `position` attribute (0.5 =
  cell-centred, 0 = nodal, per axis) gives `Centering`, including the face-
  and edge-centred E/B fields. Physical coordinates come from
  `gridGlobalOffset`, `gridSpacing` and `gridUnitSI` (and `unitSI` for
  values).
- **Iterations.** This is the biggest change outside `src/io`. One openPMD
  series holds many iterations, but `SequenceController` treats each frame as
  a `std::filesystem::path`. A frame needs to become a (path, iteration) pair,
  and the GUI, the remote open request and `DatasetId` all need to carry it.
- **Ranges.** Plotfile FAB headers store per-block min/max, which makes the
  "File" colour range cheap. openPMD has no equivalent, so the range comes
  from a scan, or `rangeAvailable` returns false until a scan has run.
- **Particles.** These map well to openPMD particle species (`position` plus
  `positionOffset`, `id`, other records). But `ParticlePoint::id` is
  documented as AMReX's `idcpu`, and `ParticleAttribute` refers to components
  by their position in the plotfile Header's list. Both need format-neutral
  wording.
- **Unsupported for openPMD:** the mapped grid (no `Nu_nd`) and standalone
  FABs. The existing `supports*()` flags on `DatasetSession` already handle
  this.

## Risks

- **Trust boundary.** `ARCHITECTURE.md` treats every input file as potentially
  malicious, and `src/io` defends against it. HDF5 and ADIOS2 are not hardened
  against crafted files. Running `validateMetadata` before use and bounding
  chunk sizes and counts covers this code's side, but the libraries' own
  parsers remain a risk. `ARCHITECTURE.md` should say so once this lands.
- **Build and packaging.** openPMD-api brings in HDF5 and/or ADIOS2 and their
  MPI configuration. Keeping it optional protects CI and the static server
  build. The likely main use is the remote server running on the HPC system
  where the data lives, built against that site's openPMD/ADIOS2 install.

## Testing

Write small fixtures with openPMD-api's Python bindings, covering a
two-level AMR case, a staggered field, and a C-ordered layout. Check each
against an equivalent plotfile, so both readers must produce the same slices,
lines and ranges.

## Phases

1. **Refactor, no behaviour change.** *(Done.)* `AmrDataset`
   (`src/io/AmrDataset.cpp`) owns the cache, IO lock and derived fields over
   a `BlockSource` (`include/amrexplorer/io/BlockSource.hpp`);
   `PlotfileSource` is the plotfile implementation, and `PlotfileDataset` is
   now only its two constructors. The queries, `DatasetPage` and
   `LocalDatasetSession` take `AmrDataset`.
2. **openPMD mesh reader, one iteration.** *(Done.)* `OpenPmdSource`
   (`src/io/openpmd/`) behind `AMREXPLORER_ENABLE_OPENPMD`, with a pinned
   FetchContent fallback built without MPI. `readDatasetMetadata` and
   `openDataset` (`include/amrexplorer/io/DatasetOpen.hpp`) pick the reader
   from the path, so the GUI, the server and every frame load open a series
   with no further change. The GUI adds **File › Open openPMD Series…**. The
   tests are `test_openpmd_reader` (JSON fixtures written by openPMD-api:
   two levels, staggered fields, a transposed 2-D layout, and a line across
   both levels) and `qt_openpmd_open` (the series opened in the real window).
   Decisions and limits found along the way:
   - Staggered and nodal components are interpolated linearly to cell
     centres when read. Every field then shares one centering, which
     `validateMetadata` requires.
   - The series is opened with `verify_homogeneous_extents: false`.
     Otherwise openPMD-api 0.17 drops any staggered vector (E_x and E_y
     have different extents).
   - The finest level is the last one that every field reaches without a
     gap. A field on a different grid from the reference field is left out.
   - Written chunks become boxes, cut into tiles of at most 64³ (256² in
     2-D) so that a serial HDF5 level is not one huge block.
   - HDF5 cannot report which regions were written, so a refined level in an
     HDF5 series looks complete and shows the fill value outside what was
     written. ADIOS2 and JSON series are not affected.
   - Not handled yet: non-cartesian geometry, F-ordered meshes, particles,
     iterations beyond the first, and openPMD entries in the remote file
     browser (typing the path works).
3. **Iterations and particles.** *(Done.)* A frame is a `DatasetLocation`
   (`include/amrexplorer/io/DatasetLocation.hpp`): a path, plus an
   iteration for an openPMD series. It converts implicitly from a path, so
   plotfile callers are unchanged. `SequenceController`, `executeFrameLoad`,
   `openDataset` and `LocalDatasetSession` all take a location.
   `DatasetId` needed no change, because every frame already gets an id of
   its own.
   - **Opening a series:** the metadata of an open lists the series'
     iterations (`PlotfileMetadataResult::iterations`,
     `DatasetSession::seriesIterations`). When there is more than one, the
     open becomes a sequence of (path, iteration) frames, both locally and
     over a remote connection (`openSeriesSequence`, `openRemoteFrames`).
   - **Protocol 1.10:** `OpenDatasetRequest.iteration`/`has_iteration` and
     `DatasetOpened.series_iterations`. The client refuses to send an
     iteration to an older server, and refuses a reply that does not list
     the iteration it asked for.
   - **Particles:** read by `OpenPmdSource`. Positions are
     position + positionOffset. Particles are sampled through the shared
     `particleSelected` hash on the `id` record, and other records become
     real or int components. The particle type comments no longer assume
     AMReX.
   - **Tests:** `test_openpmd_reader` (iterations, particles),
     `test_openpmd_remote` (1.10 against an in-process server), and
     `qt_openpmd_series` / `qt_remote_openpmd_series` (a series opened in
     the real window becomes a sequence and steps).

   - **Shared series:** frames of one series share a single open `Series`
     (`src/io/openpmd/OpenPmdSeries.hpp`). It is held weakly, so it stays
     open only while some frame uses it and is reopened afresh after that.
     That matters for a series a simulation is still writing. Iterations are
     parsed only when a frame opens them (`defer_iteration_parsing`). On a
     300-iteration, 878 MB HDF5 series, opening a frame dropped from about
     63 ms to 6 ms. `testSharedSeries` checks both the sharing and the
     release.

   - **Remote file browser:** a server that reads openPMD lists series to a
     1.10 client (`DirectoryEntry.is_openpmd` and `is_file`, folded into
     1.10 because 1.10 had not been released). Files numbered as one series
     (`diag_000100.h5`, `diag_000200.h5`, ...) are listed once, as their `%T`
     pattern, which openPMD-api opens as a file-based series. An ADIOS2 `.bp`
     directory is opened rather than entered. The sequence browser shows
     series but does not let you select them. An older client sees
     directories only, as before.

   Still open:
   - Concurrent opens of different series wait for each other: the registry
     holds one lock across each open, and that wait is not cancellable.
   - A companion shows only its series' first iteration.
   - The local **Open openPMD Series...** dialog opens a single file of a
     file-per-iteration series as that one iteration. The remote browser
     already groups those files into a `%T` series; the local dialog could
     do the same.
   - A series with particles but no meshes cannot be opened.
   - Non-cartesian and F-ordered meshes are not supported.
