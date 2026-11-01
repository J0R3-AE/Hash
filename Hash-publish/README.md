# Hash YouTube Downloader

Hash is a native C++17 application for inspecting YouTube format metadata and downloading streams when YouTube exposes usable direct media URLs. It provides both a command-line interface (CLI) and an optional Qt-based graphical interface (GUI).

> **Limitations:** Hash is an experimental, best-effort downloader and is not a complete replacement for [`yt-dlp`](https://github.com/yt-dlp/yt-dlp). It does not implement every YouTube player or signature transformation, guarantee that every adaptive format can be downloaded, or bypass access controls. Public videos and playlists may work without cookies. Private, age-restricted, region-restricted, sign-in-gated, or bot-protected content may be unavailable.

## Features

- List format metadata exposed by YouTube for a video.
- Select formats by format ID, container, or quality selector.
- Download individual videos and attempt public playlist downloads.
- Use the CLI or the Qt graphical interface.
- Select a browser profile for user-agent handling.
- Optionally provide a Netscape-format cookie file or a compatible browser SQLite cookie database.
- Cache converted SQLite cookie exports for reuse.

## Using the CLI

Show the available commands:

```sh
yt-downloader --help
```

List formats for a video:

```sh
yt-downloader list 'https://www.youtube.com/watch?v=VIDEO_ID'
```

List the entries exposed for a public playlist:

```sh
yt-downloader list 'https://www.youtube.com/playlist?list=PLAYLIST_ID'
```

Download selected format IDs:

```sh
yt-downloader download 'https://youtu.be/VIDEO_ID' 18,22
```

Choose a container and target quality:

```sh
yt-downloader download 'https://youtu.be/VIDEO_ID' mp4:1080 -o ./videos
```

Use a cookie export and browser profile:

```sh
yt-downloader list 'https://youtu.be/VIDEO_ID' --browser edge --cookies cookies.txt
```

Explicitly continue without cookies:

```sh
yt-downloader list 'https://youtu.be/VIDEO_ID' --no-cookies
```

Start interactive mode:

```sh
yt-downloader interactive
```

The exact selectors and options supported by a release are shown by `yt-downloader --help`.

## Using the GUI

Open **Hash YouTube Downloader**, enter a video or playlist URL, choose a format selector, and select an output folder. Use the browser and cookie fields when a signed-in session is needed. The GUI displays command output and download status in its log area.

The GUI is designed to include the downloader functionality directly. A separate CLI executable may also be distributed as a standalone command-line tool or fallback, depending on the release package.

## Cookies and privacy

- Cookie files may contain active session credentials. Keep them private.
- Never commit cookie files, browser databases, exported sessions, or private configuration files to the repository.
- `--cookies` accepts a Netscape-format `cookies.txt` file or a supported browser SQLite cookie database/path.
- SQLite conversion does not guarantee that encrypted browser cookie values can be decrypted. Chromium-based browsers such as Chrome and Edge may protect cookie values in ways this project cannot currently read as usable cookies. Exporting a Netscape-format cookie file may be necessary.
- Selecting `--browser edge` or another browser profile changes the client profile/user-agent handling; it does not automatically extract or decrypt that browser's cookies.
- Without usable cookies, some videos and playlist entries may be unavailable because of sign-in requirements, privacy settings, age restrictions, regional restrictions, or YouTube anti-abuse checks.

## Project status

Hash is an early-stage project. YouTube can change its player behavior, metadata responses, and access checks at any time. Some formats may be listed but not downloadable, especially when their URLs require signature processing or when separate video and audio streams must be combined.

Please include the application version, operating system, command or GUI action, and relevant error output when reporting a problem. Do not include cookies or other private session data in bug reports.

## Contributing

Please read [CONTRIBUTING.md](CONTRIBUTING.md) before opening a pull request.

## License

This project is licensed under the MIT License. See [LICENSE.md](LICENSE.md) for details.
