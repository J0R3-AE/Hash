#include "ytdl/http_client.hpp"
#include "ytdl/utils.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <sqlite3.h>
#include <string>
#include <vector>

int main()
{
    using ytdl::Format;
    using ytdl::browserUserAgentForName;
    using ytdl::extractVideoId;
    using ytdl::isVideoRestrictedOrUnavailable;
    using ytdl::noCookiesWarningMessage;
    using ytdl::normalizeBrowserName;
    using ytdl::parseFormatCodes;
    using ytdl::readConfigValue;
    using ytdl::sanitizeFilename;
    using ytdl::summarizeFormatEntries;
    using ytdl::trim;

    assert(trim(" \t hello world \n") == "hello world");
    assert(trim(" \t\n").empty());

    assert(sanitizeFilename("a/b:c") == "a_b_c");
    assert(sanitizeFilename("... ") == "youtube-video");

    assert(extractVideoId("https://youtu.be/LmN63-3SUAA?si=example") ==
           "LmN63-3SUAA");
    assert(extractVideoId("https://www.youtube.com/watch?v=abc123&feature=share") ==
           "abc123");
    assert(extractVideoId("https://www.youtube.com/shorts/short-id") == "short-id");
    assert(extractVideoId("just-an-id") == "just-an-id");
    assert(extractVideoId("").empty());

    const std::vector<std::string> expected{"18", "22", "140"};
    assert(parseFormatCodes("18, 22,,140,") == expected);
    assert(parseFormatCodes(" , , ").empty());

    const std::vector<Format> sample{
        {"18", "mp4", "360p", "171.7", "30", "avc1", "mp4a.40.2", "360p", "url"},
        {"22", "mp4", "720p", "171.7", "30", "avc1", "mp4a.40.2", "720p", "url"},
        {"139", "mp4", "audio only", "48.8", "N/A", "none", "mp4a.40.5", "tiny", "url"},
        {"140", "webm", "audio only", "129.5", "N/A", "none", "opus", "tiny", "url"},
        {"248", "webm", "1080p", "156.9", "30", "vp9", "none", "1080p", "url"},
    };
    const std::vector<std::string> summary = summarizeFormatEntries(sample);
    assert(summary.size() == 5);
    assert(summary[0] == "360p/mp4");
    assert(summary[1] == "720p/mp4");
    assert(summary[2] == "1080p/webm");
    assert(summary[3] == "audio-only/mp4");
    assert(summary[4] == "audio-only/webm");

    assert(isVideoRestrictedOrUnavailable("LOGIN_REQUIRED", "Sign in to confirm you’re not a bot"));
    assert(isVideoRestrictedOrUnavailable("ERROR", "This video is unavailable in your country"));
    assert(!isVideoRestrictedOrUnavailable("OK", "Video is available"));

    assert(normalizeBrowserName("Edge") == "edge");
    assert(normalizeBrowserName("FireFox") == "firefox");
    assert(browserUserAgentForName("edge").find("Edg/") != std::string::npos);
    assert(browserUserAgentForName("firefox").find("Firefox") != std::string::npos);

    const std::string noCookiesWarning = noCookiesWarningMessage();
    assert(noCookiesWarning.find("WARNING") != std::string::npos);
    assert(noCookiesWarning.find("--cookies") != std::string::npos);

    const auto configDir = std::filesystem::temp_directory_path() / "hash-config-test";
    std::filesystem::create_directories(configDir);
    const auto configPath = configDir / ".yt-downloader.conf";
    std::ofstream configStream(configPath);
    configStream << "browser = \"edge\"\n";
    configStream << "cookies = \"/tmp/custom-cookies.txt\"\n";
    configStream << "no-cookies = true\n";
    configStream.close();

    assert(readConfigValue(configPath.string(), "browser") == "edge");
    assert(readConfigValue(configPath.string(), "cookies") == "/tmp/custom-cookies.txt");
    assert(readConfigValue(configPath.string(), "no-cookies") == "true");

    const auto tempDir = std::filesystem::temp_directory_path() /
                         "hash-cookie-import-test";
    std::filesystem::create_directories(tempDir);
    const auto sqlitePath = tempDir / "cookies.sqlite";
    const auto exportPath = tempDir / "cookies.txt";

    sqlite3* db = nullptr;
    assert(sqlite3_open(sqlitePath.string().c_str(), &db) == SQLITE_OK);
    assert(sqlite3_exec(db,
                       "PRAGMA journal_mode=WAL; CREATE TABLE moz_cookies (id INTEGER PRIMARY KEY, originAttributes TEXT NOT NULL DEFAULT '', host TEXT, path TEXT, expiry INTEGER, lastAccessed INTEGER, creationTime INTEGER, isSecure INTEGER, isHttpOnly INTEGER, inBrowserElement INTEGER DEFAULT 0, sameSite INTEGER DEFAULT 0, schemeMap INTEGER DEFAULT 0, isPartitionedAttributeSet INTEGER DEFAULT 0, updateTime INTEGER, name TEXT, value TEXT);",
                       nullptr, nullptr, nullptr) == SQLITE_OK);
    assert(sqlite3_exec(db,
                       "INSERT INTO moz_cookies(host, path, expiry, isSecure, name, value) VALUES ('.youtube.com', '/', 2147483647, 0, 'VISITOR_INFO1_LIVE', 'abc123');",
                       nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);

    std::string error;
    assert(ytdl::convertSQLiteCookiesToNetscape(sqlitePath.string(), exportPath.string(), error));
    std::ifstream exported(exportPath);
    std::string contents((std::istreambuf_iterator<char>(exported)), std::istreambuf_iterator<char>());
    assert(contents.find(".youtube.com\tTRUE\t/\tFALSE\t2147483647\tVISITOR_INFO1_LIVE\tabc123") != std::string::npos);

    std::filesystem::remove_all(tempDir);

    return 0;
}
