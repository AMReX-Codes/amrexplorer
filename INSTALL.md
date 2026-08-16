# Installing AMReXplorer

Build from source. Tested on Ubuntu 24.04 and 26.04, and on macOS.

The build produces two programs:

- `amrexplorer` — the application. Opens plotfiles on the machine it runs on.
- `amrexplorer-server` — optional. Run it where the data lives, typically an HPC
  login node; `amrexplorer` starts it and talks to it through ssh.

## Requirements

CMake 3.25 or newer, a C++20 compiler (GCC or Clang recommended), and Qt 6.4 or
newer for the application. Check CMake first — enterprise distributions and HPC
login nodes often ship an older one:

```bash
cmake --version
```

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
the `windows` preset. None of the developers uses Windows or has tested a build
on one, so there is no guidance here for it.

### Optional

`ffmpeg` is needed only to encode animation exports (MP4):

```bash
sudo apt install ffmpeg   # Ubuntu
brew install ffmpeg       # macOS
```

## Build

```bash
git clone https://github.com/amrex-codes/amrexplorer.git
cd amrexplorer
cmake --preset default
cmake --build --preset default --parallel 4
```

Run it from the build tree without installing:

```bash
./build/src/qt/amrexplorer /path/to/plotfile     # Linux
open build/src/qt/amrexplorer.app                # macOS
```

## Install

```bash
cmake --install build
```

No `sudo` needed: this installs to `~/.local` on Linux and `~/Applications` on
macOS. On Linux the application lands at `~/.local/bin/amrexplorer`, and a
desktop entry puts it in your application menu. If `~/.local/bin` did not exist
before, log out and back in for it to be on your `PATH`.

Choose somewhere else with `--prefix`:

```bash
cmake --install build --prefix /opt/amrexplorer
```

## HPC systems

Build the server only, with a GNU toolchain:

```bash
module load gcc            # or gcc-native on a Cray system
module load cmake          # if the system cmake is older than 3.25
cmake --preset remote --fresh -DCMAKE_CXX_COMPILER=g++
cmake --build --preset remote --parallel 4
cmake --install build-remote --prefix ${HOME}
```

That installs one file, `${HOME}/bin/amrexplorer-server`.

Use `--fresh` whenever you change the compiler; without it the configure fails
again with the same message and has to be run twice.

If your `${HOME}` is shared between machines, give each its own prefix so one
build does not overwrite another:

```bash
cmake --install build-remote --prefix ${HOME}/perlmutter
```

See the **[User Guide](docs/user-guide.md)** for connecting the application to
this server over SSH.

## Next steps

See the **[AMReXplorer User Guide](docs/user-guide.md)** for opening plotfiles,
navigating 2-D and 3-D data, selecting AMR levels, animation, export, and
troubleshooting. The same guide is bundled in the application under **Help >
User Guide...**.

Developers and packagers: see **[docs/building.md](docs/building.md)** for build
options, presets, the compiler matrix, and packaging an AppImage.

## Existing installations

AMReXplorer uses new application and settings identifiers. Preferences,
desktop entries, and icons created by previous releases are not migrated or
removed automatically; they can be left in place or removed manually.
