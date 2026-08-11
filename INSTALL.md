# Installing AMReXplorer

Build from source. Tested on Ubuntu 24.04 and macOS.

## Build from source

### Linux (Ubuntu)

Install the dependencies:

```bash
sudo apt install g++ cmake ninja-build qt6-base-dev
```

### Linux (all distros)

Install the dependencies with [Spack](https://github.com/spack/spack):
```bash
spack install qt-base
spack load qt-base
cmake --preset default
cmake --build --preset default
./build/src/qt/amrexplorer /path/to/plotfile
```

Remote support and the headless `amrexplorer-server` executable are included
in normal desktop builds. CMake uses an installed FlatBuffers CMake package
when one is available, otherwise it fetches the project's pinned FlatBuffers
version. Package-managed and offline builds can point `CMAKE_PREFIX_PATH` at an
installed FlatBuffers package. Set `-DAMREXPLORER_FORCE_FETCH_FLATBUFFERS=ON`
to test or explicitly select the pinned fallback.

### macOS

Install the dependencies with [Homebrew][]:

```bash
brew install cmake ninja qt
```

[Homebrew]: https://brew.sh

**Optional:** `ffmpeg` is needed to encode animation exports (MP4).

```bash
# Ubuntu
sudo apt install ffmpeg
# macOS
brew install ffmpeg
```

Build (both platforms):

```bash
git clone https://github.com/amrex-codes/amrexplorer.git
cd amrexplorer
cmake --preset default
cmake --build --preset default
```

On Linux, the executable is `build/src/qt/amrexplorer`. Run it with a plotfile:

```bash
./build/src/qt/amrexplorer /path/to/plotfile
```

On macOS, the default build is `build/src/qt/amrexplorer.app`. It can be opened
from Finder, launched with `open build/src/qt/amrexplorer.app`, or run with a
plotfile from the command line:

```bash
./build/src/qt/amrexplorer.app/Contents/MacOS/amrexplorer /path/to/plotfile
```

Install it for the current user with:

```bash
cmake --install build --prefix "$HOME/Applications"
```

Set `-DAMREXPLORER_BUILD_MACOS_APP_BUNDLE=OFF` when configuring to build the plain
`amrexplorer` executable on macOS instead. On Linux, install system-wide with:

```bash
sudo cmake --install build --prefix /usr/local
```

## Packaging an AppImage

An AppImage is a self-contained executable that runs on any modern Linux
distribution without installing packages, which makes it a convenient way to
move a build to machines you do not administer. You can build one from a source
build yourself: see
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
