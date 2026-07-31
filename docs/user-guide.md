# AMReXplorer User Guide

AMReXplorer is an interactive viewer for two- and three-dimensional AMReX
plotfiles. It also opens standalone FAB and MultiFab data. This guide assumes
you are already familiar with AMReX plotfiles, variables, and refinement
levels.

For installation and build instructions, see
[INSTALL.md](https://github.com/amrex-codes/amrexplorer/blob/main/INSTALL.md).

## Getting started

Open a dataset from the command line:

```text
amrexplorer /path/to/plotfile
```

Pass two or more plotfile directories to open them as a time sequence:

```text
amrexplorer plt00000 plt00010 plt00020
```

You can also start without a path and use the File menu:

- **Open Plotfile Directory...** opens one AMReX plotfile.
- **Open Plotfile Sequence...** opens two or more plotfiles as animation
  frames.
- **Open FAB...** opens a raw FAB data file. If the file contains several
  concatenated FAB records, all records are available in the FAB Selector.
  Headerless `_D_*` files are supported when their companion `_H` file is
  present in the same directory.
- **Open MultiFab...** opens a standalone MultiFab header.
- **Open New Window** creates an independent viewer for side-by-side
  comparison.

AMReXplorer displays 2-D and 3-D data whose FAB payloads use IEEE 32-bit or IEEE
64-bit floating-point storage.

Plotfiles written for the cylindrical RZ coordinate system, and 3-D spherical
plotfiles, open normally but are displayed on their logical grid: an RZ dataset
appears as its r-z plane with no axisymmetric weighting or revolution. **2-D
spherical (r, θ)** plotfiles are handled specially and can be shown in true
physical space — see [2-D spherical coordinates](#2-d-spherical-coordinates).

## Remote datasets

Run the headless server on the machine that can read the plotfiles. It binds
only to that machine's loopback interface and prints a per-session access
token that clients must present:

```text
$ amrexplorer-server --port 8642
LISTENING 127.0.0.1 8642 TOKEN 58f50743dff4f653b58c3a1fe5858904
```

The token is mandatory and freshly generated at every startup; there is no way
to disable it. Because loopback is shared by every user on a multi-user host,
the token — not the port — is what keeps another local user from connecting to
your server and reading data through your account. Only the terminal (and the
SSH session that carries it) ever sees the token.

On a shared machine, prefer `--port 0` so the kernel assigns a free port,
printed on the `LISTENING` line; a fixed port risks colliding with another
user's server or with a transient connection. Avoid ports in the ephemeral
range (32768–60999 on Linux), which can be briefly occupied by unrelated
outbound connections.

Forward the port over SSH from the desktop machine, then open one
server-visible path (or several paths in sequence order), supplying the token
after the endpoint as `HOST:PORT#TOKEN`:

```text
ssh -N -L 8642:127.0.0.1:8642 user@remote
amrexplorer --connect 127.0.0.1:8642#58f50743dff4f653b58c3a1fe5858904 \
    /remote/path/plt00010
```

If the remote host sits behind a login gateway (the machine you SSH to only
relays the session and serves forwards on its own loopback), forward through
the compute host directly with `-J`:

```text
ssh -N -J user@gateway -L 8642:localhost:8642 user@compute-host
```

The same workflow is available under **File > Connect to Remote Server...**,
**Open Remote Plotfile...**, and **Open Remote Plotfile Sequence...**. The
Connect dialog accepts `HOST:PORT` and then prompts for the token separately,
or you can paste `HOST:PORT#TOKEN` to supply both at once. Remote paths are
entered explicitly; protocol 1.0 does not browse the remote filesystem.
Encryption, host verification, and optional compression are provided by SSH;
the session token guards against other local users on the server host sharing
the loopback interface.

If the tunnel or server disconnects, outstanding work fails and the
Diagnostics panel reports the endpoint and status. Reconnect explicitly and
reopen the path; requests are not replayed automatically. The protocol sends
viewport-bounded slices and lines, visible Dataset-window pages, particle
samples, and clipped grid geometry. It never sends full FABs or volume field
data.

## User interface overview

![AMReXplorer displaying a three-dimensional plotfile](images/user-guide-overview.png)

The main controls are:

1. **Field and Level** select the plotted variable and AMR composition.
2. **3D Position** selects the sample index of each orthogonal slice plane.
3. **Scale** resets the zoom to the whole domain (Reset Zoom), uses a fixed
   integer zoom, and controls whether rubber-band zoom is synchronized across
   3-D panels.
4. **Range, Log, and Palette** control the mapping from values to colors.
5. **Slice panels** display the XY, XZ, and YZ planes for a 3-D dataset.
6. **Isometric view** shows the domain, grid boxes, and current slice planes.
7. **Color Scale** reports the active value-to-color mapping.
8. **Animation** controls a 3-D plane sweep or an open plotfile sequence.

Use **View** to show or hide toolbars and dock panels. Docks can be moved,
detached, resized, and placed on another side of the main window.

## Inspecting standalone FABs and MultiFabs

Opening a raw FAB file displays its first record and opens the **FAB Selector**
dock. Use its filter and table to find a record, then double-click it or press
**View FAB**. The table shows the record offset, stored box, component count,
index type, and floating-point precision.

Opening a standalone MultiFab also opens the selector, with one row for every
FAB across all of its data files. The MultiFab view excludes ghost points, as
usual. Selecting **View FAB** switches the same window to that FAB and displays
its complete stored box, including points that were ghosts in the MultiFab.
All points in this view are treated equally. Use **Back to MultiFab** to
restore the previous MultiFab field, level, slice positions, and view regions.

FAB mode uses the range of the complete selected FAB for **File** range, so
the colors do not change when a 3-D slice is moved or the view is zoomed or
panned.

Cell-centered and nodal index types are honored independently in each
direction. Coordinates, slice positions, probing, panning, and selection
snapping therefore follow the sample locations recorded by the FAB or
MultiFab.

## A basic 2-D workflow

1. Open a plotfile and choose a field from the **Field** control or
   **Variable** menu.
2. Choose **Finest available** to composite AMR levels, or choose an exact
   level when you need to inspect that level alone.
3. Left-drag around a region to zoom into it. **Scale > Sync Rubber-band
   Zoom** applies that normalized region to every 3-D panel and is enabled by
   default. Use the mouse wheel for additional panel-local display zoom.
4. Left-click a sample to inspect its coordinates, indices, level, and value in
   the status area.
5. Select an appropriate **Range** mode and palette.
6. Add grid boxes, contours, vectors, or line plots as needed.
7. Use **File > Export Image...** to save the current view.

Double-click a view, press **0**, or select **Reset Zoom** to return to the
full domain.

## Navigating and inspecting data

The active panel is the one most recently clicked or manipulated.

| Input | Action |
| --- | --- |
| Left click | Probe the value under the cursor |
| Left drag | Zoom to a rectangular subregion; optionally sync all 3-D panels |
| Shift+left drag | Pan the view |
| Arrow keys | Pan the active panel by 5 percent |
| Mouse wheel | Zoom only the panel under the pointer |
| Double click | Reset the zoom to the whole domain |
| Shift+middle click | Plot a horizontal line through the selected sample |
| Shift+right click | Plot a vertical line through the selected sample |
| Right drag | Plot a line; the drag direction chooses the orientation |
| Right click in a 3-D slice | Move the other two slice planes to the clicked point |

The line-plot window can accumulate curves, which is useful when comparing
variables, levels, or positions. Its horizontal axis uses physical coordinates
for plotfiles and integer indices for standalone FABs and MultiFabs.

Choose **View > Dataset...** or press **Ctrl+D** to inspect raw values for
the visible physical region. Values are grouped by AMR level. Clicking a
value highlights the corresponding sample in the main view.

Choose **View > Number Format...** to set the `printf`-style format used for
numeric readouts. The default is `%g`.

## Working with 3-D data

A 3-D dataset is shown as three orthogonal slices:

- **XY** has a fixed Z position.
- **XZ** has a fixed Y position.
- **YZ** has a fixed X position.

Change a plane with the X, Y, and Z index controls in **3D Position**. A right
click in any slice moves the other two planes so that all three intersect at
the selected point. Crosshairs and the isometric view show their shared
location.

Each slice panel can be navigated independently. Field, level, range,
logarithmic mapping, and palette are shared so the three panels remain
directly comparable.

The **Plane Sweep** controls in the Animation panel select an axis and step or
play through its sample indices. The speed slider controls the delay between
frames.

## Selecting fields and AMR levels

Select a field from the toolbar or the **Variable** menu.

The level controls offer:

- **Finest available** composites data from level 0 through the finest level
  that can be loaded.
- **Levs 0-N** composites levels 0 through N.
- **Level N** displays only exact level N data.

Composite views use fine data where it exists and coarser data elsewhere.
Exact-level views are useful for checking an individual level's coverage and
values.

Useful shortcuts are:

| Shortcut | Level selection |
| --- | --- |
| Ctrl+0 | Finest available composite |
| Ctrl+1 through Ctrl+9 | Composite levels 0 through N |
| Alt+0 through Alt+9 | Exact level N |

If the finest composite cannot fit in the data cache, AMReXplorer reports the
condition and retries with a lower maximum level.

## Ranges, logarithms, and palettes

The **Range** control determines which values map to the ends of the color
scale:

- **File** uses the range over the full dataset.
- **Level** uses the selected level or composite.
- **Visible** derives the range from slice data. After a full-domain slice is
  loaded, its range remains stable while you zoom and pan.
- **User** enables explicit minimum and maximum values.

If the input does not provide complete range statistics, **File** and
**Level** are unavailable and AMReXplorer uses **Visible** instead.

Range settings are remembered separately for each field while the dataset is
open. In 3-D, all three slice panels share one range.

Enable **Log** for logarithmic color mapping. The displayed range must have a
positive minimum. If it does not, AMReXplorer falls back to linear mapping and
turns **Log** off; use a positive user minimum when necessary.

Built-in palettes include rainbow, turbo, viridis, plasma, parula, coolwarm,
and blackbody. Use **View > Palette > Load Palette File...** to load a custom
`.pal` file. **View > Palette > Reverse Colormap** flips the selected palette's
color ramp (the "_r" variant, e.g. plasma_r) and stays applied as you switch
between palettes.

## Grid boxes, contours, and vectors

Press **B** or choose **View > Boxes** to show AMR grid boundaries.

Choose **View > Contours...** to select one of three display modes:

- **Raster** shows the color-mapped slice only.
- **Raster & Contours** overlays contour lines on the raster.
- **Velocity Vectors** overlays vector glyphs.

For contours, choose the number of lines and their color. For vectors, select
the U and V components for 2-D data, and the U, V, and W components for 3-D
data. AMReXplorer may propose fields based on common velocity names; verify the
component selections for your dataset.

## 2-D spherical coordinates

A 2-D plotfile whose Header records the spherical coordinate system stores its
data on a logical (r, θ) grid, where r is the radius and θ the polar angle from
the vertical axis. AMReXplorer can present it three ways, chosen under **View >
2-D Spherical > Display**:

- **R-Z (physical)** — the default. The (r, θ) data is warped into the physical
  wedge it represents, with R = r·sin θ horizontal and Z = r·cos θ vertical, so
  cell boundaries appear as circular arcs and radial lines. Grid boxes, the
  picked-cell highlight, contours, and the coordinate readout all follow the
  warp; the readout reports both physical (R, Z) and native (r, θ).
- **r-θ** — the logical grid drawn directly, r horizontal and θ vertical (the
  layout used before spherical support was added).
- **θ-r** — the same logical grid transposed, θ horizontal and r vertical.

Because the R-Z view resamples the logical grid into physical space, its curved
cell boundaries can look jagged at the native resolution. **View > 2-D Spherical
> Supersampling** sets how finely the grid is resampled (1x–16x); higher factors
trace the curves more smoothly at the cost of a larger image.

Vector glyphs are available in all three layouts. In the R-Z view each arrow is
anchored at its physical position and the (v_r, v_θ) components are rotated
into physical directions; in the r-θ and θ-r layouts the components are drawn
on the logical grid. Line plots and particle overlays are available in the r-θ
and θ-r layouts but not in the R-Z view.

## Plotfile sequences and animation

Open two or more plotfile directories with **File > Open Plotfile
Sequence...** or pass them on the command line. The Animation panel then
provides:

- a frame slider and frame number,
- the current plotfile name and simulation time,
- previous, play/pause, and next controls,
- a playback speed control.

Frame changes preserve the active field, level, range, log, palette, and
visible-region settings when those settings are valid for the next plotfile.

## Exporting images and animations

**File > Export Image...** saves the current view as either a PNG display image
or a float64 FITS data image. PNG export asks whether to include the color
scale. FITS export writes the displayed scalar samples with `BITPIX=-64`;
invalid samples are written as NaN. A 2-D export creates one image. A 3-D
export creates separate `_xy`, `_xz`, and `_yz` images. Both formats reflect
the current zoomed data region; only PNG includes visible overlays and the
optional color scale.

For an open plotfile sequence, **File > Export Animation...** writes numbered
PNG frames. If `ffmpeg` is installed and available on `PATH`, AMReXplorer also
encodes an MP4. Three-dimensional sequences produce separate output for each
orthogonal plane.

## Panels, preferences, and diagnostics

The **View** menu controls these optional panels:

- **Dataset Metadata** shows plotfile geometry, levels, variables, and related
  metadata.
- **Color Scale** shows the current numeric range and palette.
- **Diagnostics** reports request, I/O, and cache activity.
- **Animation** contains plane-sweep and sequence controls.
- **FAB Selector** lists raw FAB records or the FABs belonging to an open
  standalone MultiFab.

Window geometry, logarithmic mapping, palette, number format, and animation
speed persist across sessions.

Each open dataset has a 1 GiB data cache by default. Set
`AMREXPLORER_CACHE_SIZE_MB` to a positive number of MiB before launching to change
the initial budget:

```text
AMREXPLORER_CACHE_SIZE_MB=2048 amrexplorer /path/to/plotfile
```

Independent windows have independent datasets, caches, and view state.

## Keyboard and mouse quick reference

| Shortcut | Action |
| --- | --- |
| B | Toggle AMR grid boxes |
| 0 | Reset the zoom to the whole domain |
| 1 through 6 | Use fixed scales from 1x through 32x |
| Ctrl+0 | Composite the finest available level |
| Ctrl+1 through Ctrl+9 | Composite levels 0 through N |
| Alt+0 through Alt+9 | Show exact level N |
| Ctrl+D | Open the Dataset window |

The same interaction summary is always available from **Help > Keyboard &
Mouse...**.

## Troubleshooting

**The plotfile does not open.** Verify that the selected directory contains a
valid AMReX `Header` and its `Level_N` directories. For a sequence, every
selected path must be a plotfile.

**An initial slice reports an unsupported data format.** AMReXplorer supports
IEEE-32 and IEEE-64 FAB floating-point payloads. Integer FAB payloads and other
floating-point layouts are not supported.

**The finest level cannot be displayed.** The composite may exceed the cache
budget. Increase `AMREXPLORER_CACHE_SIZE_MB`, reduce the visible region, or select a
lower composite maximum level.

**Log turns itself off.** The selected range has a nonpositive minimum, so
AMReXplorer has fallen back to linear mapping. Select a user range with a positive
minimum and verify that the field contains positive values.

**MP4 export is skipped.** Install `ffmpeg` and make sure the executable is on
`PATH`. The PNG frames are still written.

**Controls or panels are missing.** Use the **View** menu to restore hidden
toolbars and dock panels.

For a compact reminder of the controls, choose **Help > Keyboard & Mouse...**.
