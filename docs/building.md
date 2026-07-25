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

cmake --preset debug       # Debug build → build-debug/
cmake --build --preset debug
ctest --preset debug

cmake --preset headless    # No Qt, core library + tools + tests → build-headless/
cmake --build --preset headless
ctest --preset headless

cmake --preset sanitizers  # Headless + ASan + UBSan → build-sanitizers/
cmake --build --preset sanitizers
ctest --preset sanitizers
```

The sanitizer preset enables AddressSanitizer and UndefinedBehaviorSanitizer.

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
| macOS 15 | AppleClang | GitHub CI (full Qt) |
| Windows Server 2022 | MSVC 2022 | GitHub CI (full Qt) |

All CI builds treat compiler warnings as errors. An additional headless
Ubuntu/GCC job runs the tests with AddressSanitizer and UndefinedBehaviorSanitizer.
