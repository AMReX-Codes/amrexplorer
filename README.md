<picture>
  <source media="(prefers-color-scheme: dark)" srcset="logo/amrexplorer-lockup-dark@2x.png">
  <source media="(prefers-color-scheme: light)" srcset="logo/amrexplorer-lockup@2x.png">
  <img alt="AMReXplorer" src="logo/amrexplorer-lockup@2x.png" width="740">
</picture>

AMReXplorer is a C++20 and Qt 6 desktop application for interactive visualization
of AMReX data. One executable opens 2-D and 3-D plotfiles as well as standalone
FAB and MultiFab data.

![AMReXplorer displaying a three-dimensional plotfile](docs/images/user-guide-overview.png)

## Highlights

- Demand-driven AMR reads with a bounded data cache
- IEEE-32 and IEEE-64 FAB input
- First-class raw MultiFab and FAB exploration, including per-dimension
  cell/nodal index types, ghost-cell-aware MultiFab views, multi-file FAB
  selection, and full stored-FAB inspection
- Composite and exact-level views, value probing, line plots, grid boxes,
  contours, and vector glyphs
- Three orthogonal slice views and an isometric overview for 3-D data
- Plotfile-sequence and plane-sweep animation
- Multiple palettes, logarithmic and user-defined ranges, and PNG/MP4 export

## Documentation

- **[Installation](INSTALL.md)** — dependencies, source builds, and packaged
  applications
- **[User Guide](docs/user-guide.md)** — workflows, controls, animation,
  export, shortcuts, and troubleshooting
- **[Developer Build Guide](docs/building.md)** — CMake presets, supported
  compilers, and validation

The User Guide is also bundled in the application under **Help > User
Guide...** for offline use.
