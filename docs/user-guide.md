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

`amrexplorer --version` (or `-v`) prints the version and exits, as does
`amrexplorer-server --version`. Both also take `-h` for `--help`.

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
- **Close Window** (Ctrl+W, Cmd+W on macOS) closes only the current window;
  the other windows keep running. Closing the last one exits AMReXplorer,
  which **Quit** does from any window.

Cylindrical RZ and 3-D spherical plotfiles open normally but are displayed on
their logical grid. **2-D spherical (r, θ)** plotfiles can also be shown in
true physical space — see [2-D spherical coordinates](#2-d-spherical-coordinates).

## Remote datasets

AMReXplorer can display plotfiles that live on another Linux machine, such as
an HPC login node. The client runs `amrexplorer-server` on the remote machine
through `ssh` and speaks its protocol over that ssh connection's own
input/output stream -- the way `git` and `sftp` work. No ports are opened on
either machine, no tunnel or port forwarding is involved, and the server exits
by itself as soon as the session ends. Give the client any destination that
works with the `ssh` command, including a hostname or alias from
`~/.ssh/config`:

```text
(local) $ amrexplorer --ssh user@remote-hostname /remote/path/plt00010
```

where `remote-hostname` is replaced with the actual remote hostname or alias.
Give several paths to play them as a sequence, or none to only establish the
session. The plotfile paths are named as they appear on the remote machine;
a leading `~/` expands to the home directory there, and a relative path is
resolved against it as well.

If `amrexplorer-server` is not on the non-interactive remote `PATH`, give its
path explicitly. Quote a home-relative path so `~` is expanded on the remote
machine rather than by the local shell:

```text
(local) $ amrexplorer --ssh remote-hostname --server "~/bin/amrexplorer-server" \
    /remote/path/plt00010
```

The client remembers the path for each destination, so `--server` is needed
only the first time; later connections to the same destination use it
automatically, from the CLI and the GUI alike.

The GUI equivalents are **File > Open Remote Plotfile...** and **File > Open
Remote Plotfile Sequence...**: one dialog asks for the SSH destination, the
server executable, and the plotfile path (or paths, one per line). The
connection fields are prefilled; leaving them unchanged opens further paths
over the current session, and entering a different destination starts a new
one. Instead of typing the path, **Browse...** connects (or reuses the
session) and opens a browser of the remote machine's directories, starting at
the last one browsed there or the home directory. Plotfiles are marked as
such; double-click one to open it, or select several for a sequence, which
plays in name order. Its path box accepts the same spellings as the CLI
(`~/run`, `run`, `/scratch/run`). Browsing needs a current
`amrexplorer-server` on the remote machine; with an older one the browser
reports that and the path can still be typed. Once open, a remote dataset is
driven exactly like a local one.

If the destination requires a password or keyboard-interactive MFA,
AMReXplorer opens a response dialog for each OpenSSH prompt, including the
first-connection host-key confirmation. Everything happens on one SSH
connection, so the session requires only one authentication flow and does not
rely on SSH multiplexing.

**Sharing the connection.** If `~/.ssh/config` sets `ControlMaster auto` and a
`ControlPath` for the destination, AMReXplorer's ssh becomes the master and a
later `ssh` to the same destination from a terminal rides on its connection.
Quitting AMReXplorer stops that ssh, which takes the master and every session
sharing it down too. To keep them, add `ControlPersist 10m` (or `yes`) to the
host's entry: OpenSSH then runs the master as a background process of its own,
so quitting AMReXplorer ends only its session, terminal sessions survive, and
the next AMReXplorer session reuses the connection without authenticating
again. With a duration the master closes that long after its last client
disconnects; with `yes` it stays until `ssh -O exit destination` or the
connection drops.

The client keeps the SSH process alive for the session and stops it when the
window closes; the server reads end-of-stream and exits, so nothing is left
running on the remote machine. The server binds no sockets at all in this
mode, and the session's access token is passed in memory rather than in a
process argument or shell history.

**Gateways and firewalls.** Because nothing is forwarded, the session needs no
port-forwarding permission from the remote sshd, and it works through a
bastion with `ProxyJump` in `~/.ssh/config` (or `ssh -J`) exactly as plain
`ssh` does. A gateway whose login script runs an inner `ssh` onward also works,
provided the script passes the remote command through (`exec ssh target
"$@"`); if it ignores the command and only opens an interactive shell, use a
`ProxyJump` alias instead.

**Windows.** Remote sessions are not available from a Windows client yet.

**Troubleshooting.**

- *"amrexplorer-server is installed in its PATH"*: the login shell on the
  remote machine could not find the server for a non-interactive command --
  ssh commands skip most of the shell start-up that builds an interactive
  session's `PATH`. Two fixes:
  - Pass the full path with `--server` (CLI) or in the open dialog's server
    field. It is remembered per destination, so this is a one-time entry.
  - Or make the server's directory reachable non-interactively: on the
    remote machine put the export at the *top* of `~/.bashrc`, before the
    interactivity guard (`case $- in ... esac` or `[ -z "$PS1" ] && return`)
    that most distributions ship -- lines below that guard never run for ssh
    commands. The example assumes `amrexplorer-server` is installed in
    `~/.local/bin`; use the directory it actually lives in:

    ```bash
    # ~/.bashrc on the remote machine, first line
    export PATH="$HOME/.local/bin:$PATH"
    ```

    Check with `ssh remote-hostname 'amrexplorer-server --help'`.
- *"unknown option: --stdio"*: the `amrexplorer-server` installed on the
  remote machine predates this client. Build and install a current one -- see
  [INSTALL.md](../INSTALL.md).
- The tail of the remote side's error output is included in the failure
  message, and the Diagnostics dock shows the session's state at any time.
- A shell startup file that prints output is harmless before the session
  starts (the client skips banners), but anything that writes to the
  command's standard output *after* startup -- a background job in `.bashrc`,
  for example -- corrupts the stream and ends the session.

**Slow links.** The server disconnects a client that stalls: by default it
allows 30 seconds with no write progress, and expects a response to average at
least 64 KiB/s. If a slow or intermittent link keeps dropping the connection,
relax both limits with a small wrapper script on the remote machine and pass
it as the server executable:

```text
(remote) $ cat > ~/bin/amrexplorer-server-slow <<'EOF'
#!/bin/sh
exec amrexplorer-server --write-stall-timeout-seconds 120 \
    --write-min-kib-per-second 8 "$@"
EOF
(remote) $ chmod +x ~/bin/amrexplorer-server-slow
(local)  $ amrexplorer --ssh remote-hostname \
    --server "~/bin/amrexplorer-server-slow" /remote/path/plt00010
```

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
6. **Isometric view** shows the domain, grid boxes, and current slice planes;
   **View > Volume Rendering...** opens the same view with the field
   ray-cast into it.
7. **Color Scale** reports the active value-to-color mapping.
8. **Animation** controls a 3-D plane sweep or an open plotfile sequence.

Use **View** to show or hide toolbars and dock panels. Docks can be moved,
detached, resized, and placed on another side of the main window. **View** also
holds the overlays drawn on top of the slice: grid boxes, contours and vectors,
and particles.

## Inspecting standalone FABs and MultiFabs

Opening a raw FAB file displays its first record and opens the **FAB Selector**
dock. Use its filter and table to find a record, then double-click it or press
**View FAB**. The table shows the record offset, stored box, component count,
index type, and floating-point precision.

Opening a standalone MultiFab also opens the selector, with one row for every
FAB across all of its data files. The MultiFab view excludes ghost points, as
usual. Selecting **View FAB** switches the same window to that FAB and displays
its complete stored box, including points that were ghosts in the MultiFab. Use
**Back to MultiFab** to restore the previous MultiFab field, level, slice
positions, and view regions.

FAB mode uses the range of the complete selected FAB for **File** range, so the
colors do not change when a 3-D slice is moved or the view is zoomed or panned.

Cell-centered and nodal index types are honored independently in each
direction, so coordinates, slice positions, probing, and panning follow the
sample locations recorded by the FAB or MultiFab.

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
| Arrow keys | Pan the focused panel by 5 percent (click a panel to focus it) |
| Mouse wheel | Zoom only the panel under the pointer |
| Double click | Reset the zoom to the whole domain |
| Shift+middle click | Plot a horizontal line through the selected sample |
| Shift+right click | Plot a vertical line through the selected sample |
| Right drag | Plot a line; the drag direction chooses the orientation |
| Right click in a 3-D slice | Move the other two slice planes to the clicked point |

The **Scale** control offers fixed zooms from 1x to 32x, where the factor is
screen pixels per finest-level cell. A very wide local domain cannot be shown
at finest resolution all at once, so the requested factor may not be reachable;
the Scale button then reports what it applied, such as `32x→16x`. Rubber-band
zoom is unaffected: selecting a subregion re-reads that region at finest
resolution.

The line-plot window can accumulate curves, which is useful when comparing
variables, levels, or positions. Its horizontal axis uses physical coordinates
for plotfiles and integer indices for standalone FABs and MultiFabs.

Choose **View > Dataset...** or press **Ctrl+D** to inspect raw values for
the visible physical region. Values are grouped by AMR level. Clicking a
value highlights the corresponding sample in the main view, and dragging
across a block of values highlights the region they cover. Only values a grid
covers at that level count, so selecting a whole level's table marks just the
part of the view that level provides. One level's tab holds the selection at a
time; selecting on another clears it.

Each value is drawn in the color the color bar gives it, so a number and the
pixel it stands for share one color; values past either end of the range take
that end's color, and a value the range cannot map -- a NaN, or a non-positive
value with **Log** ticked -- is drawn in magenta, as it is in the image. The
colors follow the palette and range as you change them, while the values
themselves stay as read until **Refresh**. Samples no grid covers at a level
are left blank on a darker background.

The Dataset and line-plot windows close with their **Close** button or with
Ctrl+W (Cmd+W on macOS), the same key that closes the main and volume windows.

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

## Volume rendering

**View > Volume Rendering...** opens a window that ray-casts the whole 3-D
field: every pixel accumulates the color and opacity of the cells along its
line of sight, so translucent structure inside the domain shows through. The
window has its own copy of the isometric view -- drag to rotate, wheel
to zoom, and the **XY**, **XZ**, **YZ** buttons for the axis-aligned views --
with the domain outline and the slice planes drawn over the rendered volume.
The AMR grid boxes can be drawn too, though they start off here.

The window follows the main window: the field, the AMR level, the range mode
and its User min/max, the logarithmic mapping and the palette are the ones
selected there. With a **File**, **Level** or **User** range the volume's
colors match the slices and the **Color Scale**. **Visible** is the exception: it
scales each view to what that view shows, and the volume shows the whole
domain while the slices show three planes through it -- so the same color can
mean different values in the two. Pick File or Level when you need them to
match. Its own controls set the opacity:

- **Opacity** is a curve drawn over the palette it shapes: left to right is
  the color range, bottom to top is transparent to opaque. Drag a point to
  move it, click the plot to add one, right-click a point to remove it. The
  two end points stay at the ends of the range and move only up and down, so
  the curve always covers it.

  The point you last clicked stays selected -- it is the one marked in the
  highlight color while the plot has the keyboard focus -- and the arrow keys
  move it in exact steps: one color slot sideways, one percent up and down,
  and ten times either with Shift held. Drop a point roughly where you want it
  with the mouse, then step it into place with the keys, which is easier than
  holding a drag steady and repeatable when you want the same value twice.

  It starts as a straight line from transparent at the cold end to opaque at
  the hot end. Pull the low end up to see faint values, pull a stretch down to
  look through a feature that is hiding what is behind it, or pull everything
  down except one narrow band to leave a shell at those values. A band pulled
  to the bottom is fully transparent.
- **Use palette alpha ramp** takes each color's opacity from the palette's
  own alpha ramp instead of the curve, exactly as the palette author wrote it.
  Legacy Amrvis `.pal` files carry such a ramp; the shipped palettes have a
  plain 0-100 % ramp. The two are alternatives, so the curve is greyed out
  while this is ticked.
- **Only the visible region** samples just the part of the domain the slice
  views are zoomed into, instead of all of it. The budget below is then spent
  on that part alone, so a region small enough to fit is drawn at the finest
  level's own resolution rather than a coarsened one -- which is the way to
  see fine detail in a field too large to sample whole. Each slice view
  narrows the two axes it shows, and the volume follows them as you zoom. Off
  by default; with nothing zoomed it makes no difference.
- **Quality** trades speed for detail: it sets how many samples are taken per
  cell along each ray and how many cells the field is sampled into altogether
  -- about 2 million on Draft, 17 million on Normal, 57 million on High, spread
  across the three axes in the domain's own proportions, so a long thin domain
  gets a long thin grid rather than a cube. A field finer than that is sampled
  at the coarser pitch; a level coarser than it is drawn at its own resolution.
  A remote plotfile renders the same as a local one, unless the server was
  started with a tighter ceiling of its own -- in which case **High** may look
  no different from **Normal** (see [Remote datasets](#remote-datasets)).
- **Smooth sampling** reads each point along a ray from the eight voxels
  around it instead of the one it happens to land in. It is on by default:
  reading a single voxel makes a ray fetch the same value several times over
  and renders the field as terraced blocks, which is the coarser look this
  replaces. Clear it to see the sampled voxels as they are. It costs about
  twice the march at the same number of samples per voxel -- though it buys
  more per sample, so a smooth frame at half the samples costs roughly what a
  terraced one at full does, and shows more.

  It shapes only how the volume is drawn from the grid, not how the grid is
  built from the AMR data. Where the voxel budget forces a grid coarser than
  the finest level, the detail is already gone before this applies.
- **Grid boxes** and **Domain outline** draw the AMR box edges and the edge of
  the domain over the volume. The outline is on by default and the boxes are
  not: box edges crossing a translucent field read as structure in it, which
  is worth asking for rather than having to switch off.

While the camera moves the window shows quick half-resolution drafts and
renders the full frame once it settles. Rotating and zooming reuse the field
already read, so they are much faster than the first frame; a field too large
to keep is re-read each time, which you notice as every view taking as long as
the first. The status line under the controls reports the range in use, the
sampled grid and the render time.

Volume rendering works for remote plotfiles too (see [Remote
datasets](#remote-datasets)): the server samples and renders the frame and
sends back the picture, never the field. It needs an `amrexplorer-server`
that speaks protocol 1.2; against an older server the menu item stays
disabled. **Smooth sampling** needs protocol 1.3, since the server is what
does the sampling; against a 1.2 server the box is greyed out and says so,
and the volume is rendered from the nearest voxel.

The server's `--max-volume-voxels` and `--volume-cache-mib` options set how
large one volume and one dataset's cache may get. They are per volume
and per dataset, not a total for the server, so sizing a host means multiplying
them by how many datasets and connections you allow. `--max-volume-voxels`
starts at the largest a client may ask for, so it is there to tighten a server
rather than to open one up; a request wanting more than it permits is rendered
at the lower detail rather than refused.

**File > Close** in the volume window (Ctrl+W, Cmd+W on macOS) closes the
volume window and leaves the viewer running.

**File > Export Image...** in the volume window saves the view, overlays
included, as a PNG -- what is on screen when you pick it, including the
wireframe on its own before a volume has rendered. If the name you give does
not end in `.png` one is added, and you are asked to confirm the name it will
actually be saved under.

## Selecting fields and AMR levels

Select a field from the toolbar or the **Variable** menu.

The level controls offer:

- **Finest available** composites data from level 0 through the finest level
  that can be loaded.
- **Levs 0-N** composites levels 0 through N.
- **Level N** displays only exact level N data.

A composite uses fine data where it exists and coarser data elsewhere; an
exact-level view is useful for checking one level's coverage and values.

Useful shortcuts are:

| Shortcut | Level selection |
| --- | --- |
| Ctrl+0 | Finest available composite |
| Ctrl+1 through Ctrl+9 | Composite levels 0 through N |
| Alt+0 through Alt+9 | Exact level N |

## Derived fields

**Variable > Expression Editor...** defines fields computed from the ones the
plotfile stores. Give each a name and an expression, which may run over
several lines if that reads better; **Apply**
checks the whole list and, if it holds, reopens the dataset with the new
fields, which then behave like any other field -- slices, line plots, the
volume view, the probe and export all work on them.

They appear in the field selector and the **Variable** menu below the fields
the plotfile stores, separated from them by a line, and each shows its own
expression as a tooltip.

Expressions use `+`, `-`, `*`, `/` and `**` (or the equivalent `pow(a,b)`),
with `abs`, `sqrt`, `exp`, `log`, `exp10` and `log10`, and parentheses. For
example:

```text
sqrt(x_velocity**2 + y_velocity**2)
```

A field whose name is not a plain identifier -- anything but letters, digits,
`_` and `.` -- is written `${...}`, which takes the name exactly as given:

```text
sqrt(${x-momentum}**2 + ${y-momentum}**2) / density
log10(${Y(H2)})
```

`x`, `y` and `z` are the sample's coordinates, so `sqrt(x**2 + y**2)` is a
radius field. They are the dataset's own axis coordinates: on a spherical or
cylindrical dataset they are (r, theta) or (r, z), not Cartesian. A dataset
with a field actually named `x` uses that field instead. Coordinates are
unavailable for standalone FABs and MultiFabs, which carry no physical
geometry.

An expression may also use the derived fields defined above it in the list, so
a long formula can be built in steps. Each definition is checked against the
expression when you apply it: the editor selects the definition that failed and
says what is wrong with it. Only what is wrong whatever the data is refused --
a missing or repeated name, an expression that does not parse. Reading a field
this particular dataset happens not to have is not an error; that definition is
simply greyed out here.

Other notes:

- Derived fields have no stored minimum and maximum, so the **File** and
  **Level** range modes are unavailable for them and the range starts on
  **Visible**. **User** works as usual.
- A result that is not a number -- `log` of a negative value, a division by
  zero -- is treated as missing data, like any other non-finite sample.
- The list is shared by every window of one running viewer: define a field in
  one window and it is there in the others too. It is not saved between
  sessions, so quitting forgets it and a viewer started afresh has none. A
  viewer launched separately from the command line is its own program with its
  own list.
- A definition the data in front of you cannot provide -- it reads a field this
  plotfile does not have, or this frame of a sequence does not -- is greyed out
  in the field selector and the **Variable** menu, with the reason on its
  tooltip. It is still yours, and still applies wherever it can: in another
  window on other data, or on a frame that does carry the field.
- **Import...** and **Export...** read and write the list as a JSON expression
  list, which is how a set of definitions is kept for another session. An
  import replaces what the editor is showing; nothing reaches the dataset until
  you select **Apply**.
- Derived fields are computed where the data is read. For a dataset opened from
  a remote server that means on the server: the definitions travel with the
  open, the slices come back already computed, and everything else -- the line
  plot, the Dataset window, volume rendering -- sees the computed field as it
  sees a stored one. This needs a server new enough to carry them (protocol
  1.4); against an older one the **Expression Editor** is greyed out and says
  so, and the fields the plotfile stores are unaffected.

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
`.pal` file: a legacy Amrvis sequential palette of 256 red, green and blue
bytes, optionally followed by 256 alpha bytes. AMReXplorer keeps the alpha
plane as the palette's opacity ramp for volume rendering and ignores it for
2-D slices. Each byte is a percentage, as Amrvis wrote it; if any of the
253 data slots (the first three slots are reserved) holds a byte above 100
the plane is read as 0-255 instead, and a plane whose data slots are all
zero is treated as absent. **View > Palette > Reverse Colormap** flips the selected palette's
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

## Particles

Plotfiles that carry particle data can draw it over the slice. Choose **View >
Particles...** — the item is enabled only while the open dataset has at least
one particle species.

The dialog lists every species with its particle count and three controls
each:

- **Show** draws that species, or hides it.
- **Color** picks the point color. Each species starts with a distinct default.
- **Alpha** sets opacity from 0 to 100 percent.

Below the species list are the settings that apply to all of them:

- **Visible subset** is the percentage of particles drawn, from 0.01 to 100.
- **Sampling seed** chooses *which* particles the subset contains. Change it to
  look at a different sample of the same size.
- **Point size** is the drawn diameter in pixels, from 1 to 12.
- **Only particles in cells the slice crosses** narrows each panel to the
  particles lying inside the cells that panel is showing. 3-D data only.

The same particles stay selected as you step through the frames of a sequence,
and for a given seed a lower percentage thins the same set of particles rather
than replacing it.

For 3-D data, particles are projected onto each of the three orthogonal slice
panels. By default the projection is through the whole volume: a panel shows
every selected particle, not only those near that slice plane. That reads well
on a sparse dataset and turns into a wash of points on a dense one, which is
what the **Only particles in cells the slice crosses** box is for. Ticked, a
particle is drawn only where it falls inside the cell the plane cuts, so each
panel shows one cell's thickness of particles and follows the plane as you move
it. The thickness is the cell actually drawn at that point, so a region shown at
a coarse level keeps its thicker cell rather than losing particles to a finer
level's spacing; where a panel shows no data it shows no particles either.

Particle settings are not saved between sessions. Species selection, colors,
subset percentage, seed, point size, and the slice-cell filter all reset when a
new dataset or sequence is opened; they carry across the frames of an open
sequence.

## 2-D spherical coordinates

A 2-D plotfile whose Header records the spherical coordinate system stores its
data on a logical (r, θ) grid, where r is the radius and θ the polar angle from
the vertical axis. AMReXplorer can present it three ways, chosen under **View >
2-D Spherical > Display**:

- **R-Z (physical)** — the default. The (r, θ) data is warped into the physical
  wedge it represents, with R = r·sin θ horizontal and Z = r·cos θ vertical, so
  cell boundaries appear as circular arcs and radial lines. Overlays and the
  coordinate readout follow the warp; the readout reports both physical (R, Z)
  and native (r, θ).
- **r-θ** — the logical grid drawn directly, r horizontal and θ vertical.
- **θ-r** — the same logical grid transposed, θ horizontal and r vertical.

**View > 2-D Spherical > Supersampling** sets how finely the logical grid is
resampled for the R-Z view (1x–16x); higher factors trace the curved cell
boundaries more smoothly at the cost of a larger image.

Vector glyphs are available in all three layouts. In the R-Z view each arrow is
anchored at its physical position and the (v_r, v_θ) components are rotated
into physical directions. Line plots and particle overlays are available in the
r-θ and θ-r layouts but not in the R-Z view.

## Plotfile sequences and animation

Open two or more plotfile directories with **File > Open Plotfile
Sequence...** or pass them on the command line. The Animation panel then
provides:

- a frame slider and frame number,
- the current plotfile name and simulation time,
- previous, play/pause, and next controls,
- a playback speed control.

Frame changes preserve the active field, level, range, log, palette, and
visible region when those settings remain valid for the next plotfile.
AMReXplorer refits the view when the new frame's geometry is no longer
compatible with the previous one.

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

Each open dataset has a 1 GiB data cache by default, and volume rendering fills
a second cache of the same size with the grids it samples the field into, so a
dataset you have volume-rendered can hold up to twice that. Closing the volume
window does not give that memory back -- the grids stay cached for as long as
the dataset is open, so that reopening the window draws immediately. Set
`AMREXPLORER_CACHE_SIZE_MB` to a positive number of MiB before launching to
change both:

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
| Ctrl+W | Close the window in front (the last one quits) |

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
