#include "ytdl/downloader.hpp"
#include "ytdl/http_client.hpp"
#include "ytdl/utils.hpp"
#include "ytdl/youtube_client.hpp"

#include <curl/curl.h>

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace ytdl;

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
        << "  -c, --cookies <FILE|DIR>     Use a Netscape cookies.txt or a Firefox/Chrome SQLite cookie DB\n"
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
        << "  Use --cookies with your own exported cookies.txt or a browser SQLite cookie\n"
        << "  database path. SQLite databases are converted to a temporary Netscape cookie\n"
        << "  file automatically. Keep any export private and never share it.\n"
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

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            printUsage(argc > 0 ? argv[0] : nullptr);
            return 0;
        }
        if (arg == "-c" || arg == "--cookies") {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << arg << " requires a cookies.txt path.\n";
                return 2;
            }
            cookieFile = argv[++i];
            continue;
        }
        if (arg.rfind("--cookies=", 0) == 0) {
            cookieFile = arg.substr(std::string("--cookies=").size());
            if (cookieFile.empty()) {
                std::cerr << "Error: --cookies requires a cookies.txt path.\n";
                return 2;
            }
            continue;
        }
        if (arg == "--no-cookies" || arg == "--without-cookies" || arg == "--allow-no-cookies") {
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
            std::cerr << "Usage: " << argv[0] << " login [--browser EDGE|CHROME|FIREFOX|SAFARI]\n";
            return 2;
        }

        std::cout << "Sign in to YouTube in your browser before continuing.\n\n";
        std::cout << "1. This command will open YouTube in a browser for you.\n";
        std::cout << "2. Sign in and make sure the playlist is visible in that session.\n";
        std::cout << "3. Export a Netscape-format cookies.txt from that signed-in browser session.\n";
        std::cout << "4. Re-run the tool with: --browser " << normalizeBrowserName(browserProfile)
                  << " --cookies /path/to/cookies.txt\n\n";

        const std::string openTarget = "https://www.youtube.com";
        const std::string browserHint = normalizeBrowserName(browserProfile);
        std::string shellCommand;
        if (browserHint == "edge") {
            shellCommand = "xdg-open 'https://www.youtube.com' >/dev/null 2>&1 || true";
        } else if (browserHint == "firefox") {
            shellCommand = "xdg-open 'https://www.youtube.com' >/dev/null 2>&1 || true";
        } else {
            shellCommand = "xdg-open 'https://www.youtube.com' >/dev/null 2>&1 || true";
        }
        std::system(shellCommand.c_str());

        std::cout << "Browser target: " << openTarget << "\n";
        std::cout << "When you are signed in, export the cookies to a file and then run:\n";
        std::cout << "  ./yt-downloader list 'https://www.youtube.com/playlist?list=PLAYLIST_ID' --browser "
                  << normalizeBrowserName(browserProfile) << " --cookies /path/to/cookies.txt\n";
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
        } else {
            url = positional[0];
        }
        if (url.empty()) {
            std::cerr << "Error: no URL entered.\n";
            return 2;
        }
    } else if (command == "download") {
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
    } else {
        if (positional.size() > 1) {
            std::cerr << "Usage: " << argv[0] << " interactive [URL] [-o DIR]\n";
            return 2;
        }
        if (positional.empty()) {
            std::cout << "Enter the URL of a YouTube video: ";
            std::getline(std::cin, url);
            url = trim(url);
        } else {
            url = positional[0];
        }
        if (url.empty()) {
            std::cerr << "Error: no URL entered.\n";
            return 2;
        }
    }

    if (!cookieFile.empty()) {
        std::error_code cookieError;
        const fs::path cookiePath(cookieFile);
        fs::path resolvedCookiePath = cookiePath;

        if (fs::is_directory(cookiePath, cookieError)) {
            const std::vector<std::string> candidateNames{
                "cookies.sqlite", "cookies.sqlite3", "cookies.db", "Cookies"
            };
            bool found = false;
            for (const std::string& candidate : candidateNames) {
                const fs::path candidatePath = cookiePath / candidate;
                if (fs::is_regular_file(candidatePath, cookieError) && !cookieError) {
                    resolvedCookiePath = candidatePath;
                    found = true;
                    break;
                }
            }
            if (!found) {
                std::cerr << "Error: no browser cookie database found in directory: "
                          << cookieFile << '\n';
                return 2;
            }
        } else if (!fs::is_regular_file(cookiePath, cookieError) || cookieError) {
            std::cerr << "Error: cookies file not found or not a regular file: "
                      << cookieFile << '\n';
            return 2;
        }

        std::string finalCookiePath = resolvedCookiePath.string();
        if (resolvedCookiePath.extension() == ".sqlite" ||
            resolvedCookiePath.extension() == ".db" ||
            resolvedCookiePath.extension() == ".sqlite3" ||
            resolvedCookiePath.filename() == "Cookies" ||
            resolvedCookiePath.filename() == "cookies") {
            std::string tempCookiePath = (fs::temp_directory_path() /
                ("yt-downloader-cookies-" + std::to_string(std::time(nullptr)) + ".txt")).string();
            std::string error;
            if (!convertSQLiteCookiesToNetscape(resolvedCookiePath.string(), tempCookiePath, error)) {
                std::cerr << "Error: failed to convert browser SQLite cookie database: "
                          << error << '\n';
                return 2;
            }
            finalCookiePath = tempCookiePath;
        }

        setCookieFile(fs::absolute(finalCookiePath).string());
    } else if (allowNoCookies || cookieFile.empty()) {
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
        const int result = downloadPlaylist(url, formatCodes, outputDirectory, playlistError);
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
        std::cout << "\nPlaylist entries exposed to the anonymous public feed (" << playlist.videoIds.size()
                  << "):\n";
        std::cout << "Note: the browser page may show more content if some videos are private, region-locked, or require sign-in.\n";
        for (std::size_t i = 0; i < playlist.videoIds.size(); ++i)
            std::cout << "  " << (i + 1) << ". https://youtu.be/" << playlist.videoIds[i] << '\n';
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
        std::cout << "\nEnter format IDs or selectors (e.g. 18,22 or mp4:1080 or mp3:380k): ";
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
        if (pathError)
            std::cerr << " " << pathError.message();
        std::cerr << '\n';
        curl_global_cleanup();
        return 1;
    }

    const int failures = downloadFormats(title, formats, formatCodes, outputDirectory);
    curl_global_cleanup();
    return failures == 0 ? 0 : 1;
}
