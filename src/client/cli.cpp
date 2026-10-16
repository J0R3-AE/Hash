#include "ytdl/downloader.hpp"
#include "ytdl/http_client.hpp"
#include "ytdl/utils.hpp"
#include "ytdl/youtube_client.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
using namespace ytdl;

namespace {

// ============================================================
// Path and SQLite helpers
// ============================================================

fs::path expandHomePath(const std::string& rawPath)
{
    if (rawPath.empty() || rawPath[0] != '~' ||
        (rawPath.size() > 1 && rawPath[1] != '/' && rawPath[1] != '\\')) {
        return fs::path(rawPath);
    }

    const char* home = std::getenv("HOME");
#ifdef _WIN32
    if (!home || !*home) {
        home = std::getenv("USERPROFILE");
    }
#endif

    if (!home || !*home) {
        return fs::path(rawPath);
    }

    if (rawPath.size() == 1) {
        return fs::path(home);
    }

    return fs::path(home) / rawPath.substr(2);
}

std::string lowercase(std::string value)
{
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
    return value;
}

bool hasSQLiteHeader(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }

    char header[16] = {};
    input.read(header, sizeof(header));

    return input.gcount() == static_cast<std::streamsize>(sizeof(header)) &&
           std::string(header, 15) == "SQLite format 3" &&
           header[15] == '\0';
}

bool looksLikeSQLiteCookieDatabase(const fs::path& path)
{
    if (hasSQLiteHeader(path)) {
        return true;
    }

    const std::string extension = lowercase(path.extension().string());
    const std::string filename = lowercase(path.filename().string());

    // If a file is named like a database but its header is invalid,
    // still route it to the converter so it reports a useful error.
    return extension == ".sqlite" ||
           extension == ".sqlite3" ||
           extension == ".db" ||
           filename == "cookies";
}

// ============================================================
// Cookie conversion cache
// ============================================================

std::uint64_t stableHash(const std::string& value)
{
    // FNV-1a is used only to create a compact cache filename. The
    // original path and source metadata are also stored in the
    // fingerprint to detect a collision or changed database.
    std::uint64_t hash = 14695981039346656037ULL;

    for (unsigned char c : value) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }

    return hash;
}

fs::path cookieCacheDirectory()
{
#ifdef _WIN32
    const char* localAppData = std::getenv("LOCALAPPDATA");
    if (localAppData && *localAppData) {
        return fs::path(localAppData) /
               "HashYouTubeDownloader" / "CookieCache";
    }

    const char* userProfile = std::getenv("USERPROFILE");
    if (userProfile && *userProfile) {
        return fs::path(userProfile) /
               "AppData" / "Local" /
               "HashYouTubeDownloader" / "CookieCache";
    }
#else
    const char* xdgCache = std::getenv("XDG_CACHE_HOME");
    if (xdgCache && *xdgCache) {
        return fs::path(xdgCache) /
               "hash-youtube-downloader" / "cookies";
    }

    const char* home = std::getenv("HOME");
    if (home && *home) {
        return fs::path(home) / ".cache" /
               "hash-youtube-downloader" / "cookies";
    }
#endif

    std::error_code ec;
    const fs::path temporaryDirectory = fs::temp_directory_path(ec);
    if (!ec) {
        return temporaryDirectory / "yt-downloader-cookie-cache";
    }

    return fs::current_path() / ".yt-downloader-cookie-cache";
}

bool makeCookieFingerprint(
    const fs::path& source,
    fs::path& absoluteSource,
    std::string& fingerprint,
    std::string& error)
{
    std::error_code ec;
    absoluteSource = fs::absolute(source, ec);

    if (ec) {
        error = "Cannot resolve cookie database path: " + ec.message();
        return false;
    }

    absoluteSource = absoluteSource.lexically_normal();

    std::ostringstream state;
    state << absoluteSource.generic_string() << '\n';

    // Chromium and other SQLite users may have pending transactions
    // in the WAL file. Include sidecar metadata in the cache key.
    const std::vector<fs::path> files = {
        absoluteSource,
        fs::path(absoluteSource.string() + "-wal"),
        fs::path(absoluteSource.string() + "-shm")
    };

    for (const fs::path& file : files) {
        ec.clear();
        const bool exists = fs::exists(file, ec);

        if (ec) {
            error = "Cannot inspect cookie database '" + file.string() +
                    "': " + ec.message();
            return false;
        }

        state << file.filename().string() << ':';
        if (!exists) {
            state << "missing\n";
            continue;
        }

        const auto size = fs::file_size(file, ec);
        if (ec) {
            error = "Cannot read cookie database size for '" +
                    file.string() + "': " + ec.message();
            return false;
        }

        const auto modified = fs::last_write_time(file, ec);
        if (ec) {
            error = "Cannot read cookie database timestamp for '" +
                    file.string() + "': " + ec.message();
            return false;
        }

        state << size << ':'
              << modified.time_since_epoch().count() << '\n';
    }

    fingerprint = state.str();
    return true;
}

bool getCachedNetscapeCookieFile(
    const fs::path& source,
    fs::path& cachedPath,
    std::string& error)
{
    fs::path absoluteSource;
    std::string fingerprint;

    if (!makeCookieFingerprint(source, absoluteSource, fingerprint, error)) {
        return false;
    }

    const fs::path cacheDirectory = cookieCacheDirectory();
    std::error_code ec;

    fs::create_directories(cacheDirectory, ec);
    if (ec) {
        error = "Cannot create cookie cache directory '" +
                cacheDirectory.string() + "': " + ec.message();
        return false;
    }

#ifndef _WIN32
    // The cache holds authentication cookies. Keep it private to
    // the current user on Unix-like systems, including WSL.
    if (::chmod(cacheDirectory.c_str(), S_IRWXU) != 0) {
        error = "Cannot secure cookie cache directory: " +
                cacheDirectory.string();
        return false;
    }
#endif

    const std::string cacheName =
        "cookies-" + std::to_string(stableHash(absoluteSource.generic_string()));

    const fs::path cacheFile = cacheDirectory / (cacheName + ".txt");
    const fs::path metadataFile = cacheDirectory / (cacheName + ".meta");

    bool cacheFileValid = false;
    ec.clear();

    if (fs::is_regular_file(cacheFile, ec) && !ec) {
        const auto size = fs::file_size(cacheFile, ec);
        cacheFileValid = !ec && size > 0;
    }

    if (cacheFileValid) {
        std::ifstream metadataInput(metadataFile, std::ios::binary);

        if (metadataInput) {
            const std::string oldFingerprint = std::string(
                std::istreambuf_iterator<char>(metadataInput),
                std::istreambuf_iterator<char>());

            if (oldFingerprint == fingerprint) {
                cachedPath = cacheFile;
                std::cout << "Using cached cookie conversion.\n";
                return true;
            }
        }
    }

    std::cout << "Converting SQLite cookie database...\n";

    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());

    fs::path temporaryFile = cacheFile;
    temporaryFile += ".tmp-" + suffix;

    if (!convertSQLiteCookiesToNetscape(
            absoluteSource.string(), temporaryFile.string(), error)) {
        std::error_code ignored;
        fs::remove(temporaryFile, ignored);
        return false;
    }

    ec.clear();
    if (!fs::is_regular_file(temporaryFile, ec) || ec) {
        error = "Cookie conversion did not produce an output file.";
        std::error_code ignored;
        fs::remove(temporaryFile, ignored);
        return false;
    }

    const auto convertedSize = fs::file_size(temporaryFile, ec);
    if (ec || convertedSize == 0) {
        error = "Cookie conversion produced an empty output file.";
        std::error_code ignored;
        fs::remove(temporaryFile, ignored);
        return false;
    }

#ifdef _WIN32
    // std::filesystem::rename cannot replace an existing file on
    // Windows. This is a disposable cache, so remove it first.
    fs::remove(cacheFile, ec);
    ec.clear();
#endif

    fs::rename(temporaryFile, cacheFile, ec);
    if (ec) {
        error = "Cannot save converted cookies to cache '" +
                cacheFile.string() + "': " + ec.message();
        std::error_code ignored;
        fs::remove(temporaryFile, ignored);
        return false;
    }

#ifndef _WIN32
    if (::chmod(cacheFile.c_str(), S_IRUSR | S_IWUSR) != 0) {
        error = "Cannot secure converted cookie cache file: " +
                cacheFile.string();
        return false;
    }
#endif

    // Write metadata through a temporary file so a partial metadata
    // write doesn't produce a cache that appears valid.
    fs::path temporaryMetadata = metadataFile;
    temporaryMetadata += ".tmp-" + suffix;

    bool metadataWritten = false;
    {
        std::ofstream output(
            temporaryMetadata,
            std::ios::binary | std::ios::trunc);

        if (output) {
            output.write(
                fingerprint.data(),
                static_cast<std::streamsize>(fingerprint.size()));
            output.close();
            metadataWritten = static_cast<bool>(output);
        }
    }

    if (metadataWritten) {
#ifdef _WIN32
        fs::remove(metadataFile, ec);
        ec.clear();
#endif
        fs::rename(temporaryMetadata, metadataFile, ec);

        if (ec) {
            std::error_code ignored;
            fs::remove(temporaryMetadata, ignored);
        }
#ifndef _WIN32
        else if (::chmod(metadataFile.c_str(), S_IRUSR | S_IWUSR) != 0) {
            // Failure to secure metadata isn't a reason to use a
            // different cookie source, but don't reuse this cache.
            std::error_code ignored;
            fs::remove(metadataFile, ignored);
        }
#endif
    }
    else {
        std::error_code ignored;
        fs::remove(temporaryMetadata, ignored);
    }

    cachedPath = cacheFile;
    return true;
}

} // namespace

static void printUsage(const char* executable)
{
    const char* name = (executable && *executable) ? executable : "yt-downloader";
    std::cout
        << "Native YouTube format lister and downloader\n\n"
        << "Usage:\n"
        << "  " << name << " <command> [arguments] [options]\n\n"
        << "Commands:\n"
        << "  list [URL]                  List formats available to this client\n"
        << "  download <URL> <SELECTORS>  Download IDs or playlist entries\n"
        << "  interactive [URL]           List formats and prompt for IDs/extensions\n"
        << "  login                       Open YouTube in a browser and walk through sign-in\n\n"
        << "Options:\n"
        << "  -o, --output <DIR>           Output directory (default: current directory)\n"
        << "  -c, --cookies <FILE|DIR>     Use Netscape cookies.txt or a supported SQLite cookie DB\n"
        << "  --no-cookies                 Continue without cookies and show a warning\n"
        << "  --browser <NAME>             Browser profile to mimic: auto, chrome, edge, firefox, safari\n"
        << "  -h, --help                   Show this help text\n"
        << "  --version                    Show version information\n\n"
        << "Examples:\n"
        << "  " << name << " list\n"
        << "  " << name << " list 'https://www.youtube.com/watch?v=VIDEO_ID'\n"
        << "  " << name << " download 'https://youtu.be/VIDEO_ID' 18,22\n"
        << "  " << name << " download 'https://youtu.be/VIDEO_ID' mp4\n"
        << "  " << name << " download 'https://youtu.be/VIDEO_ID' mp4:1080\n"
        << "  " << name << " download 'https://youtu.be/VIDEO_ID' mp3:380k\n"
        << "  " << name << " download 'https://youtu.be/VIDEO_ID' mp4,mp4  # best two distinct MP4 streams\n"
        << "  " << name << " download 'https://youtu.be/VIDEO_ID' mp4,webm\n"
        << "  " << name << " download 'https://www.youtube.com/playlist?list=PLAYLIST_ID' mp4:1080 -o ./videos\n"
        << "  " << name << " download 'https://youtu.be/VIDEO_ID' 18,22 -o ./videos\n"
        << "  " << name << " list 'https://youtu.be/VIDEO_ID' --cookies cookies.txt\n"
        << "  " << name << " list 'https://youtube.com/playlist?list=PLAYLIST_ID' --browser edge --cookies ~/cookies.txt\n"
        << "  " << name << " list 'https://youtu.be/VIDEO_ID' --no-cookies\n"
        << "  " << name << " login --browser edge\n"
        << "  " << name << " interactive 'https://youtu.be/VIDEO_ID'\n"
        << "  " << name << " interactive\n\n"
        << "Notes:\n"
        << "  This tool uses native C++ networking (libcurl) and Boost.PropertyTree.\n"
        << "  It discovers the current desktop WEB player configuration dynamically.\n"
        << "  SQLite cookie databases are converted and cached for reuse.\n"
        << "  Some Chromium-based databases contain encrypted cookie values that this\n"
        << "  converter cannot decrypt; export a Netscape cookies.txt if required.\n"
        << "  Keep any cookie export private and never share it.\n"
        << "  If you intentionally run without cookies, use --no-cookies and expect a\n"
        << "  warning because public videos may work while private or region-locked content\n"
        << "  will be unavailable.\n"
        << "  Use --browser edge or --browser chrome to match the browser profile used\n"
        << "  when exporting your cookies, especially for browser-based sign-in flows.\n"
        << "  Some formats may be unavailable when YouTube requires protected URL or\n"
        << "  JavaScript signature handling not implemented by this client.\n";
}

int main(int argc, char* argv[])
{
    if (argc < 2) {
        printUsage(argc > 0 ? argv[0] : nullptr);
        return 2;
    }

    const std::string command = argv[1];
    if (command == "-h" || command == "--help" || command == "help") {
        printUsage(argc > 0 ? argv[0] : nullptr);
        return 0;
    }
    if (command == "--version" || command == "version") {
        std::cout << "yt-downloader 1.3.0 (native C++)\n";
        return 0;
    }

    if (command != "list" && command != "download" && command != "interactive" &&
        command != "login") {
        std::cerr << "Error: unknown command '" << command << "'.\n\n";
        printUsage(argc > 0 ? argv[0] : nullptr);
        return 2;
    }

    std::vector<std::string> positional;
    fs::path outputDirectory = fs::current_path();
    std::string cookieFile;
    std::string browserProfile = "auto";
    bool allowNoCookies = false;

    const fs::path configPath = fs::current_path() / ".yt-downloader.conf";
    const char* homeEnvironment = std::getenv("HOME");
#ifdef _WIN32
    if (!homeEnvironment || !*homeEnvironment) {
        homeEnvironment = std::getenv("USERPROFILE");
    }
#endif
    const fs::path homeConfigPath =
        fs::path(homeEnvironment ? homeEnvironment : "") / ".yt-downloader.conf";
    const fs::path activeConfigPath = fs::exists(configPath)
        ? configPath
        : (fs::exists(homeConfigPath) ? homeConfigPath : fs::path{});

    if (!activeConfigPath.empty()) {
        const std::string configBrowser =
            readConfigValue(activeConfigPath.string(), "browser");
        const std::string configCookies =
            readConfigValue(activeConfigPath.string(), "cookies");
        const std::string configNoCookies =
            readConfigValue(activeConfigPath.string(), "no-cookies");

        if (browserProfile == "auto" && !configBrowser.empty()) {
            browserProfile = configBrowser;
        }
        if (cookieFile.empty() && !configCookies.empty()) {
            cookieFile = configCookies;
        }
        if (!allowNoCookies && configNoCookies == "true") {
            allowNoCookies = true;
        }
    }

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            printUsage(argc > 0 ? argv[0] : nullptr);
            return 0;
        }

        if (arg == "-c" || arg == "--cookies") {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << arg << " requires a cookie file or directory.\n";
                return 2;
            }
            cookieFile = argv[++i];
            continue;
        }

        if (arg.rfind("--cookies=", 0) == 0) {
            cookieFile = arg.substr(std::string("--cookies=").size());
            if (cookieFile.empty()) {
                std::cerr << "Error: --cookies requires a cookie file or directory.\n";
                return 2;
            }
            continue;
        }

        if (arg == "--no-cookies" || arg == "--without-cookies" ||
            arg == "--allow-no-cookies") {
            allowNoCookies = true;
            continue;
        }

        if (arg == "--browser") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --browser requires a browser name such as auto, chrome, edge, firefox, or safari.\n";
                return 2;
            }
            browserProfile = argv[++i];
            continue;
        }

        if (arg.rfind("--browser=", 0) == 0) {
            browserProfile = arg.substr(std::string("--browser=").size());
            if (browserProfile.empty()) {
                std::cerr << "Error: --browser requires a browser name such as auto, chrome, edge, firefox, or safari.\n";
                return 2;
            }
            continue;
        }

        if (arg == "-o" || arg == "--output") {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << arg << " requires a directory.\n";
                return 2;
            }
            outputDirectory = argv[++i];
            continue;
        }

        if (arg.rfind("--output=", 0) == 0) {
            const std::string value = arg.substr(std::string("--output=").size());
            if (value.empty()) {
                std::cerr << "Error: --output requires a directory.\n";
                return 2;
            }
            outputDirectory = value;
            continue;
        }

        if (!arg.empty() && arg[0] == '-') {
            std::cerr << "Error: unknown option '" << arg << "'.\n";
            return 2;
        }

        positional.push_back(arg);
    }

    std::string url;
    std::vector<std::string> formatCodes;

    if (command == "login") {
        if (!positional.empty()) {
            std::cerr << "Usage: " << argv[0]
                      << " login [--browser EDGE|CHROME|FIREFOX|SAFARI]\n";
            return 2;
        }

        std::cout << "Sign in to YouTube in your browser before continuing.\n\n";
        std::cout << "1. This command will open YouTube in a browser for you.\n";
        std::cout << "2. Sign in and make sure the playlist is visible in that session.\n";
        std::cout << "3. Export a Netscape-format cookies.txt from that signed-in browser session.\n";
        std::cout << "4. Re-run the tool with: --browser " << normalizeBrowserName(browserProfile)
                  << " --cookies /path/to/cookies.txt\n\n";

        const std::string openTarget = "https://www.youtube.com";
        const std::string shellCommand =
            "xdg-open 'https://www.youtube.com' >/dev/null 2>&1 || true";
        std::system(shellCommand.c_str());

        std::cout << "Browser target: " << openTarget << "\n";
        std::cout << "When you are signed in, export the cookies to a file and then run:\n";
        std::cout << "  ./yt-downloader list 'https://www.youtube.com/playlist?list=PLAYLIST_ID' --browser "
                  << normalizeBrowserName(browserProfile)
                  << " --cookies /path/to/cookies.txt\n";
        return 0;
    }

    if (command == "list") {
        if (positional.size() > 1) {
            std::cerr << "Usage: " << argv[0] << " list [URL]\n";
            return 2;
        }
        if (positional.empty()) {
            std::cout << "Enter the URL of a YouTube video or playlist: ";
            std::getline(std::cin, url);
            url = trim(url);
        }
        else {
            url = positional[0];
        }
        if (url.empty()) {
            std::cerr << "Error: no URL entered.\n";
            return 2;
        }
    }
    else if (command == "download") {
        if (positional.size() != 2) {
            std::cerr << "Usage: " << argv[0]
                      << " download <URL> <ID|EXT|EXT:QUALITY[,ID|EXT|EXT:QUALITY...]> [-o DIR]\n";
            return 2;
        }
        url = positional[0];
        formatCodes = parseDownloadSelectors(positional[1]);
        if (formatCodes.empty()) {
            std::cerr << "Error: provide at least one selector, e.g. 18,22 or mp4,webm or mp4:1080 or mp3:380k.\n";
            return 2;
        }
    }
    else {
        if (positional.size() > 1) {
            std::cerr << "Usage: " << argv[0] << " interactive [URL] [-o DIR]\n";
            return 2;
        }
        if (positional.empty()) {
            std::cout << "Enter the URL of a YouTube video: ";
            std::getline(std::cin, url);
            url = trim(url);
        }
        else {
            url = positional[0];
        }
        if (url.empty()) {
            std::cerr << "Error: no URL entered.\n";
            return 2;
        }
    }

    if (!cookieFile.empty()) {
        std::error_code cookieError;
        const fs::path cookiePath = expandHomePath(cookieFile);
        fs::path resolvedCookiePath = cookiePath;

        if (fs::is_directory(cookiePath, cookieError) && !cookieError) {
            const std::vector<std::string> candidateNames = {
                "cookies.sqlite", "cookies.sqlite3", "cookies.db", "Cookies"
            };

            bool found = false;
            for (const std::string& candidate : candidateNames) {
                const fs::path candidatePath = cookiePath / candidate;
                cookieError.clear();
                if (fs::is_regular_file(candidatePath, cookieError) && !cookieError) {
                    resolvedCookiePath = candidatePath;
                    found = true;
                    break;
                }
            }

            if (!found) {
                std::cerr << "Error: no browser cookie database found in directory: "
                          << cookiePath.string() << '\n';
                return 2;
            }
        }
        else {
            cookieError.clear();
            if (!fs::is_regular_file(cookiePath, cookieError) || cookieError) {
                std::cerr << "Error: cookies file not found or not a regular file: "
                          << cookiePath.string() << '\n';
                return 2;
            }
        }

        std::string finalCookiePath = resolvedCookiePath.string();

        if (looksLikeSQLiteCookieDatabase(resolvedCookiePath)) {
            fs::path cachedCookiePath;
            std::string error;

            if (!getCachedNetscapeCookieFile(
                    resolvedCookiePath,
                    cachedCookiePath,
                    error)) {
                std::cerr
                    << "Error: failed to convert browser SQLite cookie database: "
                    << error << '\n';
                return 2;
            }

            finalCookiePath = cachedCookiePath.string();
        }

        std::error_code absoluteError;
        const fs::path absoluteCookiePath =
            fs::absolute(fs::path(finalCookiePath), absoluteError);

        if (absoluteError) {
            std::cerr << "Error: cannot resolve cookies file path: "
                      << absoluteError.message() << '\n';
            return 2;
        }

        setCookieFile(absoluteCookiePath.string());
    }
    else {
        std::cout << noCookiesWarningMessage() << '\n';
    }

    setBrowserProfile(browserProfile);

    const CURLcode curlInit = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (curlInit != CURLE_OK) {
        std::cerr << "Error: failed to initialize networking.\n";
        return 1;
    }

    if (command == "download" && isPlaylistUrl(url)) {
        std::string playlistError;
        const int result = downloadPlaylist(
            url, formatCodes, outputDirectory, playlistError);

        if (result < 0) {
            std::cerr << "Error: " << playlistError << '\n';
        }

        curl_global_cleanup();
        return result < 0 ? 1 : (result == 0 ? 0 : 1);
    }

    if (command == "list" && isPlaylistUrl(url)) {
        PlaylistFetchResult playlist;

        if (!fetchPlaylist(url, playlist)) {
            std::cerr << "Error: " << playlist.error << '\n';
            curl_global_cleanup();
            return 1;
        }

        std::cout
            << "\nPlaylist entries exposed to the anonymous public feed ("
            << playlist.videoIds.size() << "):\n";
        std::cout
            << "Note: the browser page may show more content if some videos are private, "
               "region-locked, or require sign-in.\n";

        for (std::size_t i = 0; i < playlist.videoIds.size(); ++i) {
            std::cout << "  " << (i + 1)
                      << ". https://youtu.be/"
                      << playlist.videoIds[i] << '\n';
        }

        curl_global_cleanup();
        return 0;
    }

    VideoFetchResult video;
    if (!fetchVideo(url, video)) {
        std::cerr << "Error: " << video.error << '\n';
        curl_global_cleanup();
        return 1;
    }

    std::string title = video.title;
    std::vector<Format> formats = std::move(video.formats);

    if (command == "list") {
        listFormats(formats);
        curl_global_cleanup();
        return 0;
    }

    if (command == "interactive") {
        listFormats(formats);

        std::string formatInput;
        std::cout
            << "\nEnter format IDs or selectors (e.g. 18,22 or mp4:1080 or mp3:380k): ";
        std::getline(std::cin, formatInput);

        formatCodes = parseDownloadSelectors(formatInput);
        if (formatCodes.empty()) {
            std::cerr << "Error: no format selectors entered.\n";
            curl_global_cleanup();
            return 2;
        }
    }

    std::error_code pathError;
    fs::create_directories(outputDirectory, pathError);

    if (pathError || !fs::is_directory(outputDirectory)) {
        std::cerr << "Error: cannot create/access output directory '"
                  << outputDirectory.string() << "'.";
        if (pathError) {
            std::cerr << " " << pathError.message();
        }
        std::cerr << '\n';
        curl_global_cleanup();
        return 1;
    }

    const int failures = downloadFormats(
        title, formats, formatCodes, outputDirectory);

    curl_global_cleanup();
    return failures == 0 ? 0 : 1;
}
