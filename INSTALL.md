# Installing AMReXplorer

Build from source. Tested on Ubuntu 24.04 and 26.04, and on macOS; for Windows
see [below](#windows). There are no prebuilt binaries yet.

The build produces two independent executables.

`amrexplorer` is the application. On its own it opens plotfiles stored on the
machine it runs on, which is the ordinary case and needs nothing further.

`amrexplorer-server` is optional. Run it on the machine where the data lives —
typically an HPC login node — and connect to it from `amrexplorer` over an SSH
tunnel, for datasets too large or too awkward to copy back. If you only ever
open local files you can ignore it; a desktop build produces it anyway, and it
costs nothing to leave alone.

| | needs at run time | typical home |
| --- | --- | --- |
| `amrexplorer` | Qt 6 and its dependencies | your desktop or laptop |
| `amrexplorer-server` | the C and C++ system libraries — on Linux usually the C library alone, see [HPC](#hpc-systems-the-server-only) | an HPC login node |

They install and move independently.

## Dependencies

### Linux (Ubuntu)

```bash
sudo apt install g++ cmake ninja-build qt6-base-dev
```

### Linux (any distribution)

With [Spack](https://github.com/spack/spack):

```bash
spack install qt-base
spack load qt-base
```

### macOS

With [Homebrew](https://brew.sh):

```bash
brew install cmake ninja qt
```

### Windows

Windows is covered only in continuous integration: MSVC 2022 with Qt 6.8, using
the `windows` preset, building and running the test suite on every change.

None of the developers uses Windows or has tested a build on a Windows machine,
so there is no guidance here on installing Qt, choosing an install location, or
anything else specific to the platform. The CI job is evidence that the code
compiles and the tests pass; it is not a supported installation path.

### Optional

`ffmpeg` is needed only to encode animation exports (MP4):

```bash
sudo apt install ffmpeg   # Ubuntu
brew install ffmpeg       # macOS
```

FlatBuffers is found automatically: CMake uses an installed package when there
is one and otherwise fetches the pinned version. Point `CMAKE_PREFIX_PATH` at an
installed package for offline or package-managed builds, or set
`-DAMREXPLORER_FORCE_FETCH_FLATBUFFERS=ON` to force the pinned fallback.

## Build

```bash
git clone https://github.com/amrex-codes/amrexplorer.git
cd amrexplorer
cmake --preset default
cmake --build --preset default
```

Run it straight from the build tree, without installing:

```bash
./build/src/qt/amrexplorer /path/to/plotfile                      # Linux
open build/src/qt/amrexplorer.app                                 # macOS
```

On macOS the build is an `.app` bundle; to pass a plotfile on the command line
use `./build/src/qt/amrexplorer.app/Contents/MacOS/amrexplorer`, or configure
with `-DAMREXPLORER_BUILD_MACOS_APP_BUNDLE=OFF` for a plain executable.

## Install

```bash
cmake --install build
```

No `sudo`, and no prefix to choose: the default is the per-user location for
your platform — `~/.local` on Linux, and `~/Applications` on macOS when building
the `.app` bundle, which is the default there. (CMake's own default is
`/usr/local`, which most people building this cannot write to.) Building on
macOS with `-DAMREXPLORER_BUILD_MACOS_APP_BUNDLE=OFF` produces a plain
executable rather than a bundle, and installs to `~/.local` like Linux.

On Linux that installs:

```
~/.local/bin/amrexplorer
~/.local/bin/amrexplorer-server
~/.local/share/applications/amrexplorer.desktop
~/.local/share/icons/hicolor/{16,32,64,128,256}x*/apps/amrexplorer.png
```

`~/.local/bin` is on `PATH` by default on most distributions, and the desktop
entry and icons make AMReXplorer appear in your application menu. The
application also writes its own desktop entry on first run, so the menu entry
works even if you skip installing and just copy the executable somewhere.

On macOS it installs `~/Applications/amrexplorer.app`, plus
`~/Applications/bin/amrexplorer-server`.

Choose somewhere else with `--prefix`:

```bash
cmake --install build --prefix /opt/amrexplorer      # needs write access
```

## HPC systems: the server only

On a cluster you want the server and nothing else. Use the `remote` preset,
which builds no Qt and installs exactly one file:

```bash
cmake --preset remote
cmake --build --preset remote
cmake --install build-remote --prefix ${HOME}
# -> ${HOME}/bin/amrexplorer-server
```

Three things are worth knowing.

**The system compiler is usually too old.** HPC login nodes run an OS several
years old, and `/usr/bin/c++` with it, so a default configure often fails on
C++20 support. The compiler you want is a module away:

```bash
module load gcc
cmake --preset remote --fresh -DCMAKE_CXX_COMPILER=g++
```

On Cray systems such as Perlmutter and Frontier, use the compiler wrapper:

```bash
cmake --preset remote --fresh -DCMAKE_CXX_COMPILER=CC
```

`--fresh` is not optional after a failed configure. That configure has already
written a cache naming the old compiler, and while CMake normally clears such a
cache and retries within the same run, the C++20 error aborts before it can — so
without `--fresh` the next attempt fails identically, naming the old compiler
again, and only a third would succeed.

Configuring with an unsupported compiler fails with a message naming the
compiler it found and repeating these commands.

**The binary is easy to move.** On Linux the server links the C++ runtime
statically and depends only on the C library, so you can build it once and copy
it where you like — including to machines where the compiler module you built
with is not loaded. It does need a glibc no older than the machine that built
it, so build on the cluster rather than carrying a binary from your laptop.
Packagers who want the system runtime should configure with
`-DAMREXPLORER_SERVER_STATIC_CXX_RUNTIME=OFF`.

**Beware a shared home directory.** Several centres share `${HOME}` across
machines. Installing to the same `${HOME}/bin` from two clusters overwrites one
build with the other, and the survivor may not run on both. Give each system its
own prefix:

```bash
cmake --install build-remote --prefix ${HOME}/perlmutter
```

See the **[User Guide](docs/user-guide.md)** for connecting the desktop
application to a running server over an SSH tunnel.

## Development tools

`amrexplorer-render-equivalence`, which compares local and remote rendering, is
built but not installed by default. Install it deliberately with:

```bash
cmake --install build --component tools
```

## Packaging an AppImage

An AppImage is a self-contained executable that runs on any modern Linux
distribution without installing packages, which makes it a convenient way to
move a build to machines you do not administer. Build one from a source build
yourself: see
**[Building an AppImage](docs/building.md#building-an-appimage)**.

There is no release pipeline yet, so no prebuilt AppImage is published.

## Next steps

See the **[AMReXplorer User Guide](docs/user-guide.md)** for opening plotfiles,
navigating 2-D and 3-D data, selecting AMR levels, animation, export, and
troubleshooting. The same guide is bundled in the application under **Help >
User Guide...**.

## Existing installations

AMReXplorer uses new application and settings identifiers. Preferences,
desktop entries, and icons created by previous releases are not migrated or
removed automatically; they can be left in place or removed manually.
