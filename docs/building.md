# Building AMReXplorer

AMReXplorer requires a C++20 compiler, CMake 3.25 or newer, and Ninja for the
provided presets. Qt 6.4 or newer is required only by the desktop target.
There is no build-time dataset dimension: one executable reads 2-D and 3-D
data, and the same runtime metadata representation accepts 1-D data.

## Presets

```text
cmake --preset default     # Release build → build/
cmake --build --preset default
ctest --preset default

cmake --preset clang       # Release with Clang, warnings as errors → build-clang/
cmake --build --preset clang
ctest --preset clang

cmake --preset debug       # Debug build → build-debug/
cmake --build --preset debug
ctest --preset debug

cmake --preset headless    # Release, no Qt, core + tools + tests → build-headless/
cmake --build --preset headless
ctest --preset headless

cmake --preset remote      # Release headless server + protocol tests → build-remote/
cmake --build --preset remote
ctest --preset remote

cmake --preset sanitizers  # Debug headless + ASan + UBSan → build-sanitizers/
cmake --build --preset sanitizers
ctest --preset sanitizers

cmake --preset tsan        # Debug headless + remote + TSan → build-tsan/
cmake --build --preset tsan
ctest --preset tsan

cmake --preset qt-sanitizers  # Qt Debug + ASan + UBSan → build-qt-sanitizers/
cmake --build --preset qt-sanitizers
ctest --preset qt-sanitizers
```

The sanitizer preset enables AddressSanitizer and UndefinedBehaviorSanitizer.
The tsan preset enables ThreadSanitizer (mutually exclusive with the ASan
build), covering the concurrent block-read, cache, and stop-token paths. It
also enables remote support, so the client receiver thread, per-session reader
threads, and the shared server worker pool are checked by the `remote_*` tests
in the same run. The qt-sanitizers preset runs the full suite — including the
offscreen Qt smoke tests and, because Qt implies remote, the remote tests —
under ASan/UBSan with leak detection; the headless `sanitizers` preset stays
local-only to avoid duplicating that coverage. A `windows` Release preset
(usable only on Windows hosts) mirrors the CI MSVC job.

The clang preset mirrors the CI clang job (which builds with
`-DAMREXPLORER_WARNINGS_AS_ERRORS=ON`): run it before pushing to catch
Clang-only diagnostics that a GCC build stays silent about, such as
`-Wunused-const-variable`.

Remote support is enabled by default with Qt and can be selected explicitly
with `-DAMREXPLORER_ENABLE_REMOTE=ON`. The checked-in FlatBuffers schema is
compiled into the build tree; generated bindings are not checked in. CMake
first searches for an installed FlatBuffers config package that provides
`flatbuffers::flatbuffers` and `flatbuffers::flatc`. If none is available, it
fetches the pinned fallback. Use `CMAKE_PREFIX_PATH` to select a package-manager
installation, or `-DAMREXPLORER_FORCE_FETCH_FLATBUFFERS=ON` to force the
fallback path.

On macOS, Qt builds produce `build/src/qt/amrexplorer.app` by default. The bundle
contains its executable at `Contents/MacOS/amrexplorer` and can be installed into a
user application directory with:

```bash
cmake --install build --prefix "$HOME/Applications"
```

Configure with `-DAMREXPLORER_BUILD_MACOS_APP_BUNDLE=OFF` to retain the plain
`build/src/qt/amrexplorer` executable layout.

## Building an AppImage

From the repository root on Linux, build AMReXplorer and install it into an
AppDir:

```bash
cmake --preset default
cmake --build --preset default
DESTDIR="$(pwd)/appdir" cmake --install build --prefix /usr
```

Download `linuxdeploy` and its Qt plugin if they are not already present:

```bash
wget -O linuxdeploy.AppImage \
  https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
wget -O linuxdeploy-plugin-qt.AppImage \
  https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage
chmod +x linuxdeploy.AppImage linuxdeploy-plugin-qt.AppImage
```

Bundle the application. AMReXplorer uses Qt Widgets but no QML, so QML scanning is
disabled:

```bash
export QMAKE=/usr/bin/qmake6
OUTPUT=amrexplorer.AppImage QML_SOURCES_PATHS=. \
./linuxdeploy.AppImage --appdir appdir \
  --executable appdir/usr/bin/amrexplorer \
  --desktop-file resources/amrexplorer.desktop \
  --icon-file resources/amrexplorer.png \
  --output appimage \
  --plugin qt
```

## Optional VTK module

`AMREXPLORER_ENABLE_VTK` is currently deliberately unavailable. It remains off
until AMReXplorer has both a Qt 6-compatible VTK configuration and a bounded
volume-query contract. Enabling the option causes CMake to stop with an
explanation instead of linking a Qt 5-based system VTK into the Qt 6
application.

## Compiler matrix

| Platform | Compiler | State |
|---|---|---|
| Ubuntu 24.04 | GCC | GitHub CI (full Qt) |
| Ubuntu 24.04 | Clang | GitHub CI (full Qt) |
| Ubuntu 26.04 | GCC 15 | Developer machines (full Qt) |
| macOS 15 | AppleClang | GitHub CI (full Qt) |
| Windows Server 2022 | MSVC 2022 | GitHub CI (full Qt) |

All CI builds treat compiler warnings as errors. An additional headless
Ubuntu/GCC job runs the tests with AddressSanitizer and UndefinedBehaviorSanitizer.
