# Contributing

Thanks for helping improve this project.

## Development setup

Requirements:

- C++17 compiler
- GNU Make
- `pkg-config`
- libcurl development headers
- SQLite development headers
- Boost headers

On Debian or Ubuntu:

```sh
sudo apt install build-essential pkg-config libcurl4-openssl-dev libsqlite3-dev libboost-dev
```

Build and validate:

```sh
make
make test
make gui
```

## Project structure

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
    client/
      http.cpp
      youtube.cpp
    download/
      playlist.cpp
      video.cpp
    format/
      parser.cpp
      parser.hpp
      selector.cpp
  cookie.cpp
  utils.cpp

tests/
  test_utils.cpp
  test_format_selector.cpp
```

The `client` layer is for CLI and GUI entry points. The `core` layer contains the actual networking, parsing, format selection, and download logic.

## Contribution guidelines

- Keep UI and CLI wiring out of the core logic.
- Keep shared declarations in `include/ytdl` and implementation details in the matching source files.
- Keep private helper code inside the relevant `.cpp` file unless it is intentionally shared.
- Check and report HTTP, parsing, and filesystem failures explicitly.
- Do not commit cookies, exported browser session files, or other private session data.
- Prefer focused pull requests with a clear explanation of changes and tested behavior.

## Behavior expectations

This project intentionally behaves like a best-effort YouTube metadata downloader. It does not emulate a full logged-in browser or bypass every anti-abuse control.

Important constraints:

- Without cookies, the playlist and video logic usually only sees the anonymous public feed.
- Private, age-gated, or region-locked items may not appear unless the user provides a valid browser session.
- Some formats may be unavailable because YouTube does not expose direct URLs or the client is blocked by server-side checks.

## Pull request checklist

Before submitting a patch, please confirm:

- the project still builds with `make`
- the relevant tests still pass with `make test`
- the change is scoped and descriptive
- limitations or warnings are documented where relevant
