# Hash YouTube Downloader

A small C++17 application for inspecting YouTube format metadata and downloading streams when YouTube exposes usable direct URLs.

> **Limitations:** This is a best-effort downloader, not a complete replacement for [`yt-dlp`](https://github.com/yt-dlp/yt-dlp). It does not implement every YouTube player/signature transformation, guarantee every adaptive format can be downloaded, or bypass access controls. Public videos and playlists may work without cookies. Private, age-restricted, region-restricted, or bot-protected content may be unavailable.

## Features

- CLI for listing available formats and downloading selected streams.
- Optional Qt 5 GUI.
- Best-effort public playlist enumeration.
- Browser-profile-aware user-agent handling.
- Optional Netscape-format cookie files and support for converting compatible browser SQLite cookie databases.
- Makefile-based Linux build and MXE Windows cross-build.

## Requirements

### Linux CLI

- C++17 compiler (such as GCC or Clang)
- GNU Make
- `pkg-config`
- libcurl development headers/libraries
- SQLite development headers/libraries
- Boost headers (Boost.PropertyTree is used for JSON parsing)

### Linux GUI

The GUI additionally requires Qt 5 Widgets development files.

On Debian/Ubuntu, install the dependencies with:

```sh
sudo apt update
sudo apt install build-essential pkg-config libcurl4-openssl-dev libsqlite3-dev libboost-dev qtbase5-dev
```

If you only need the CLI and don't want to install Qt, use `make cli` instead of the default `make` target.

## Build on Linux

Run these commands from the repository root:

```sh
# Build CLI and GUI
make

# Build only the CLI
make cli

# Build the CLI and GUI
make gui

# Build and run the Linux tests
make test
```

The executables are written to:

```text
build/linux/bin/yt-downloader
build/linux/bin/yt-downloader-gui
```

## Build for Windows with MXE

The Windows cross-build uses MXE's static target, `x86_64-w64-mingw32.static`. MXE must be installed at `~/Code/mxe` by default, and its target must have the required static curl, SQLite, Boost, Qt 5, and Qt Windows platform-plugin development files. Set `MXE_ROOT` or `MXE_TARGET` on the make command line to override the defaults.

```sh
# Build Windows CLI and GUI
make windows

# Build Linux and Windows
make all-platforms

# Put the Windows executables into a distribution directory
make package-windows
```

Windows build outputs are written to `build/windows/`. The package target copies the executables to:

```text
build/windows/package/
```

The intended package contains `yt-downloader.exe` and `yt-downloader-gui.exe`. A static GUI build depends on a matching static Qt installation and a correctly linked/imported `QWindowsIntegrationPlugin`; a successful link alone does not guarantee the program works on every Windows machine. Check the DLL imports and test the package on a Windows system that does not have MXE's DLL directories on `PATH` before distributing it.

If compiling MXE packages under WSL causes the environment to run out of resources, reduce parallel jobs (for example, `-j2`) rather than using every reported CPU core.

## Usage

### CLI

Examples below assume the Linux executable at `build/linux/bin/yt-downloader`.

```sh
# Show command help
./build/linux/bin/yt-downloader --help

# List video formats
./build/linux/bin/yt-downloader list 'https://www.youtube.com/watch?v=VIDEO_ID'

# List a playlist
./build/linux/bin/yt-downloader list 'https://youtube.com/playlist?list=PLAYLIST_ID'

# Download selected format IDs
./build/linux/bin/yt-downloader download 'https://youtu.be/VIDEO_ID' 18,22

# Save downloads into a directory
./build/linux/bin/yt-downloader download 'https://youtu.be/VIDEO_ID' 18,22 -o ./videos

# Use a Netscape-format cookies file
./build/linux/bin/yt-downloader list 'https://youtu.be/VIDEO_ID' --cookies cookies.txt

# Make an explicitly unauthenticated request
./build/linux/bin/yt-downloader list 'https://youtu.be/VIDEO_ID' --no-cookies

# Start interactive mode
./build/linux/bin/yt-downloader interactive
```

See `./build/linux/bin/yt-downloader --help` for the options supported by the current build.

### GUI

```sh
./build/linux/bin/yt-downloader-gui
```

## Cookies and privacy

- `--cookies` accepts a Netscape-format `cookies.txt` file or a supported browser SQLite cookie database/path.
- SQLite conversion does not guarantee that encrypted browser cookie values can be decrypted. Chrome/Edge profiles may store protected values that this project cannot currently read as usable cookies.
- Cookies can contain active session credentials. Never commit cookie files, browser databases, exported sessions, or private configuration files.
- Without usable cookies, some videos and playlist entries may be unavailable due to sign-in requirements, region restrictions, age restrictions, or YouTube anti-abuse checks.

## Source layout

```text
include/ytdl/
  downloader.hpp
  format_selector.hpp
  http_client.hpp
  types.hpp
  utils.hpp
  youtube_client.hpp

src/client/
  cli.cpp
  gui.cpp

src/core/
  client/
    http.cpp
    youtube.cpp
  cookie.cpp
  download/
    playlist.cpp
    video.cpp
  format/
    parser.cpp
    parser.hpp
    selector.cpp
  utils.cpp

tests/
  test_utils.cpp
  test_format_selector.cpp

Makefile
README.md
CONTRIBUTING.md
LICENSE.md
```

The `src/client/` directory contains the CLI and GUI entry points. Shared networking, cookie handling, parsing, format selection, and download operations live under `src/core/`. Public declarations are in `include/ytdl/`.

## Contributing

Please read [CONTRIBUTING.md](CONTRIBUTING.md) before opening a pull request.

## License

This project is licensed under the MIT License. See [LICENSE.md](LICENSE.md).
