# Hash YouTube Downloader

A small C++17 project for inspecting YouTube format metadata and downloading streams when YouTube exposes direct URLs.

> Important: this project is intentionally limited. It does not implement full YouTube playback circumvention, heavy DRM workarounds, or authenticated browser sessions by default. Public playlist and video access may work without cookies, but private, age-gated, and region-locked content will usually fail unless you provide a usable browser session export. This is not a complete replacement for `yt-dlp`.

## Features

- CLI downloader and format listing
- optional Qt GUI
- playlist enumeration through the anonymous public feed
- browser-profile-aware user-agent handling
- optional cookie file or SQLite browser cookie conversion
- Makefile-based build

## Requirements

- C++17 compiler
- GNU Make
- `pkg-config`
- libcurl development headers
- SQLite development headers
- Boost headers for JSON parsing

On Debian/Ubuntu:

```sh
sudo apt install build-essential pkg-config libcurl4-openssl-dev libsqlite3-dev libboost-dev
```

## Build

```sh
make
make test
```

The executable is created in `bin/yt-downloader`.

## Usage

CLI:

```sh
./bin/yt-downloader --help
./bin/yt-downloader list 'https://www.youtube.com/watch?v=VIDEO_ID'
./bin/yt-downloader list 'https://youtube.com/playlist?list=PLAYLIST_ID'
./bin/yt-downloader download 'https://youtu.be/VIDEO_ID' 18,22
./bin/yt-downloader download 'https://youtu.be/VIDEO_ID' 18,22 -o ./videos
./bin/yt-downloader list 'https://youtu.be/VIDEO_ID' --cookies cookies.txt
./bin/yt-downloader list 'https://youtu.be/VIDEO_ID' --no-cookies
./bin/yt-downloader interactive
```

GUI:

```sh
make gui
./build/yt-downloader-gui
```

## Cookie and privacy notes

- `--cookies` accepts a Netscape-style `cookies.txt` or a browser SQLite cookie database.
- SQLite browser databases are converted to a temporary Netscape export automatically.
- Cookie files may contain session credentials. Never commit them or share them.
- Without cookies, the app can only see the public anonymous feed. Some videos and playlists will be missing or unavailable.

## Source layout

```text
include/
  ytdl/
    types.hpp
    utils.hpp
    http_client.hpp
    youtube_client.hpp
    downloader.hpp
    format_selector.hpp

src/
  client/
    cli.cpp
    gui.cpp
  core/
    downloader.cpp
    format_parser.cpp
    format_parser.hpp
    format_selector.cpp
    http_client.cpp
    utils.cpp
    youtube_client.cpp

tests/
  test_utils.cpp
  test_format_selector.cpp
```

The project is split into a client layer and a core layer. The client handles CLI/GUI entry points, while the core contains the networking, parsing, selection, and downloader logic.

## Contributing

Please read [CONTRIBUTING.md](CONTRIBUTING.md) before opening a pull request.

## License

This project is licensed under the MIT License. See [LICENSE.md](LICENSE.md) for details.
# Hash
