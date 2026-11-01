# Contributing to Hash

Thanks for helping improve Hash YouTube Downloader. Focused fixes, clear bug reports, and small, testable pull requests are welcome.

## Development setup

The project uses C++17 and GNU Make. The Linux CLI requires `pkg-config`, libcurl development files, SQLite development files, and Boost headers. The GUI additionally requires Qt 5 Widgets.

On Debian/Ubuntu:

```sh
sudo apt update
sudo apt install build-essential pkg-config libcurl4-openssl-dev libsqlite3-dev libboost-dev qtbase5-dev
```

Build and test from the repository root:

```sh
make cli       # build only the CLI
make           # build CLI and GUI
make test      # build and run the Linux tests
```

Linux executables are produced in `build/linux/bin/`. Windows cross-compilation uses MXE's static target and is documented in [README.md](README.md#build-for-windows-with-mxe).

## Project structure

```text
include/ytdl/       Public/shared headers
src/client/         CLI and Qt GUI entry points
src/core/client/    HTTP and YouTube client implementation
src/core/download/  Video and playlist download operations
src/core/format/    Format parsing and selection
src/core/           Cookie handling and shared utilities
tests/              Unit tests
```

The client layer should handle command-line or GUI wiring. Keep reusable networking, parsing, selection, and download logic in the core layer. Put shared declarations in `include/ytdl/` and implementation-specific helper functions in the relevant `.cpp` file unless they need to be shared.

## Contribution guidelines

- Keep UI and CLI-specific behavior out of core logic where practical.
- Use clear names and follow the existing C++17 style.
- Check and report HTTP, parsing, filesystem, and process errors explicitly.
- Avoid silently choosing a lower-quality format when the user requested a specific quality.
- Keep changes focused and explain the reason for behavior changes.
- Update the README when commands, dependencies, options, output paths, or known limitations change.
- Add or update tests for format-selection logic and other behavior that can be tested without network access.
- Run `make test` before submitting changes when possible, and report any tests you could not run.

## Behavior expectations and limitations

This project is a best-effort YouTube metadata downloader. It does not emulate every logged-in browser behavior or circumvent every server-side restriction. Direct stream URLs may be unavailable, and some advertised formats may require signature processing or merging separate audio/video streams that the project may not implement.

Without usable cookies, private, age-restricted, region-restricted, or bot-protected videos may fail to load. Do not represent these limitations as guaranteed bugs when reporting issues; include the command used and the relevant error output, while removing private information.

## Security and privacy

- Never commit cookies, browser databases, exported sessions, `.yt-downloader.conf`, credentials, tokens, or other private data.
- Review `git diff --cached` before committing to make sure no downloads, generated binaries, build directories, or secrets are staged.
- Do not include personal account details or sensitive cookie values in bug reports.

## Pull request checklist

Before opening a pull request, please confirm:

- [ ] The change is scoped and has a descriptive commit/PR message.
- [ ] The project builds with the relevant target (`make cli`, `make`, or `make windows`).
- [ ] Relevant tests pass with `make test`, or any failure is described.
- [ ] Documentation reflects changed behavior or commands.
- [ ] No generated executables, build outputs, downloaded media, cookies, or credentials are included.
