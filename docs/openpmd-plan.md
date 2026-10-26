# Reading openPMD output — implementation plan

A proposed design, drafted on 2026-10-26, for opening openPMD series
(HDF5 or ADIOS2) alongside AMReX plotfiles. The *Phases* section is the
suggested order of PRs; phase 1 is done.

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
2. **openPMD mesh reader, one iteration.** `OpenPmdSource` for meshes only,
   behind `AMREXPLORER_ENABLE_OPENPMD`, with reader selection on open and the
   comparison fixtures.
3. **Iterations and particles.** Frames as (path, iteration) through
   `SequenceController`, `DatasetId` and the remote protocol; openPMD particle
   species, with the format-neutral `ParticlePoint`/`ParticleAttribute`
   wording.
