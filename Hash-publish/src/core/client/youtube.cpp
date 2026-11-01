
#include "ytdl/youtube_client.hpp"

#include "../format/parser.hpp"
#include "ytdl/http_client.hpp"
#include "ytdl/utils.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using boost::property_tree::ptree;

namespace ytdl {
namespace {

constexpr const char* kPlayerEndpoint =
    "https://www.youtube.com/youtubei/v1/player?prettyPrint=false";

constexpr auto kClientCacheDuration =
    std::chrono::minutes(30);

constexpr auto kFailedConfigCacheDuration =
    std::chrono::seconds(30);

// ============================================================
// String / JSON helpers
// ============================================================

std::string jsonQuote(const std::string& value)
{
    static constexpr char hex[] = "0123456789abcdef";

    std::string result;
    result.reserve(value.size() + 2);
    result += '"';

    for (unsigned char c : value) {
        switch (c) {
        case '"':
            result += "\\\"";
            break;

        case '\\':
            result += "\\\\";
            break;

        case '\b':
            result += "\\b";
            break;

        case '\f':
            result += "\\f";
            break;

        case '\n':
            result += "\\n";
            break;

        case '\r':
            result += "\\r";
            break;

        case '\t':
            result += "\\t";
            break;

        default:
            if (c < 0x20) {
                result += "\\u00";
                result += hex[(c >> 4) & 0x0f];
                result += hex[c & 0x0f];
            }
            else {
                result += static_cast<char>(c);
            }
        }
    }

    result += '"';
    return result;
}

// Read one quoted JSON string beginning at openingQuote.
// Returns an empty string if the string is malformed.
std::string readJsonString(
    const std::string& text,
    std::size_t openingQuote)
{
    if (openingQuote >= text.size() ||
        text[openingQuote] != '"') {
        return {};
    }

    std::string result;
    bool escaped = false;

    for (std::size_t i = openingQuote + 1;
         i < text.size(); ++i) {
        const char c = text[i];

        if (escaped) {
            switch (c) {
            case '"':
            case '\\':
            case '/':
                result += c;
                break;

            case 'b':
                result += '\b';
                break;

            case 'f':
                result += '\f';
                break;

            case 'n':
                result += '\n';
                break;

            case 'r':
                result += '\r';
                break;

            case 't':
                result += '\t';
                break;

            default:
                // These extracted configuration values and video
                // IDs should not need Unicode escape decoding.
                // Preserve an unknown escape rather than silently
                // dropping its contents.
                result += '\\';
                result += c;
                break;
            }

            escaped = false;
            continue;
        }

        if (c == '\\') {
            escaped = true;
            continue;
        }

        if (c == '"') {
            return result;
        }

        result += c;
    }

    return {};
}

// Extract a string field from a JSON object or JSON-like section.
std::string extractJsonStringField(
    const std::string& json,
    const std::string& key)
{
    const std::string marker =
        "\"" + key + "\"";

    std::size_t position = 0;

    while ((position = json.find(marker, position))
           != std::string::npos) {
        std::size_t colon =
            json.find(':', position + marker.size());

        if (colon == std::string::npos) {
            return {};
        }

        std::size_t valuePosition = colon + 1;

        while (valuePosition < json.size() &&
               std::isspace(
                   static_cast<unsigned char>(
                       json[valuePosition]))) {
            ++valuePosition;
        }

        if (valuePosition < json.size() &&
            json[valuePosition] == '"') {
            return readJsonString(json, valuePosition);
        }

        position = colon + 1;
    }

    return {};
}

std::string extractConfigString(
    const std::string& html,
    const std::string& key)
{
    return extractJsonStringField(html, key);
}

std::string lowercase(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

    return value;
}

bool isValidVideoId(const std::string& id)
{
    if (id.size() != 11) {
        return false;
    }

    return std::all_of(
        id.begin(),
        id.end(),
        [](unsigned char c) {
            return std::isalnum(c)
                || c == '_'
                || c == '-';
        });
}

// Find the matching closing brace for a JSON object.
// Braces inside quoted strings are ignored.
std::size_t findJsonObjectEnd(
    const std::string& text,
    std::size_t openingBrace)
{
    if (openingBrace >= text.size() ||
        text[openingBrace] != '{') {
        return std::string::npos;
    }

    std::size_t depth = 0;
    bool inString = false;
    bool escaped = false;

    for (std::size_t i = openingBrace;
         i < text.size(); ++i) {
        const char c = text[i];

        if (inString) {
            if (escaped) {
                escaped = false;
            }
            else if (c == '\\') {
                escaped = true;
            }
            else if (c == '"') {
                inString = false;
            }

            continue;
        }

        if (c == '"') {
            inString = true;
        }
        else if (c == '{') {
            ++depth;
        }
        else if (c == '}') {
            if (--depth == 0) {
                return i;
            }
        }
    }

    return std::string::npos;
}

// ============================================================
// YouTube player client profiles
// ============================================================

std::vector<ClientProfile> buildPlayerClients(
    bool& discoveredCurrentWebConfig)
{
    discoveredCurrentWebConfig = false;

    std::vector<ClientProfile> clients;

    const std::string webUserAgent =
        browserUserAgentForName(getBrowserProfileName());

    std::string homepage;
    std::string homepageError;

    if (httpGetText(
            "https://www.youtube.com/",
            homepage,
            homepageError)) {
        const std::string apiKey =
            extractConfigString(
                homepage,
                "INNERTUBE_API_KEY");

        const std::string clientVersion =
            extractConfigString(
                homepage,
                "INNERTUBE_CLIENT_VERSION");

        if (!apiKey.empty() && !clientVersion.empty()) {
            clients.push_back({
                "WEB",
                clientVersion,
                "1",
                apiKey,
                webUserAgent,
                "\"clientScreen\":\"WATCH\","
                "\"platform\":\"DESKTOP\""
            });

            discoveredCurrentWebConfig = true;
        }
        else {
            std::cerr
                << "Warning: could not find the current WEB "
                   "client configuration; trying fallback "
                   "player profiles.\n";
        }
    }
    else {
        std::cerr
            << "Warning: could not fetch YouTube's homepage "
               "to discover the WEB client configuration ("
            << homepageError
            << "); trying fallback player profiles.\n";
    }

    // Fallback profiles. These values are retained from the
    // existing project; they may need updating as YouTube changes.
    clients.push_back({
        "VISIONOS",
        "1.02",
        "101",
        "AIzaSyB-63vPrdThhKuerbB2N_l7Kwwcxj6yUAc",
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) "
        "AppleWebKit/605.1.15 (KHTML, like Gecko) "
        "Version/26.0 Safari/605.1.15",
        "\"deviceMake\":\"Apple\","
        "\"deviceModel\":\"RealityDevice17,1\","
        "\"osName\":\"visionOS\","
        "\"osVersion\":\"26.5.23O471\""
    });

    clients.push_back({
        "IOS",
        "21.26.4",
        "5",
        "AIzaSyB-63vPrdThhKuerbB2N_l7Kwwcxj6yUAc",
        "com.google.ios.youtube/21.26.4 "
        "(iPhone16,2; U; CPU iOS 18_3_2 like Mac OS X;)",
        "\"deviceMake\":\"Apple\","
        "\"deviceModel\":\"iPhone16,2\","
        "\"osName\":\"iPhone\","
        "\"osVersion\":\"18.3.2.22D82\""
    });

    clients.push_back({
        "ANDROID",
        "21.26.364",
        "3",
        "AIzaSyA8eiZmM1FaDVjRy-df2KTyQ_vz_yYM39w",
        "com.google.android.youtube/21.26.364 "
        "(Linux; U; Android 11) gzip",
        "\"androidSdkVersion\":30,"
        "\"osName\":\"Android\","
        "\"osVersion\":\"11\""
    });

    return clients;
}

// Cache the player profiles so a playlist does not require a new
// homepage request for every video. If discovery fails, retry it
// after a short interval instead of caching the failure for long.
std::vector<ClientProfile> playerClients()
{
    static std::mutex cacheMutex;
    static std::vector<ClientProfile> cachedClients;

    static std::chrono::steady_clock::time_point refreshAt{};

    std::lock_guard<std::mutex> lock(cacheMutex);

    const auto now =
        std::chrono::steady_clock::now();

    if (!cachedClients.empty() && now < refreshAt) {
        return cachedClients;
    }

    bool currentWebConfig = false;

    cachedClients =
        buildPlayerClients(currentWebConfig);

    const auto completedAt =
        std::chrono::steady_clock::now();

    refreshAt = completedAt;

    if (currentWebConfig) {
        refreshAt += kClientCacheDuration;
    }
    else {
        refreshAt += kFailedConfigCacheDuration;
    }

    return cachedClients;
}

// ============================================================
// YouTube player API
// ============================================================

bool requestPlayerResponse(
    const std::string& videoId,
    const ClientProfile& client,
    ptree& playerResponse,
    std::string& error)
{
    const std::string extraFields =
        client.extraClientFields.empty()
            ? std::string()
            : client.extraClientFields + ",";

    const std::string body =
        "{\"context\":{\"client\":{"
        "\"clientName\":" + jsonQuote(client.name) + ","
        "\"clientVersion\":" + jsonQuote(client.version) + ","
        "\"userAgent\":" + jsonQuote(client.userAgent) + ","
        + extraFields
        + "\"hl\":\"en\",\"gl\":\"US\"}},"
        "\"videoId\":" + jsonQuote(videoId) + ","
        "\"contentCheckOk\":true,"
        "\"racyCheckOk\":true}";

    const std::string endpoint =
        std::string(kPlayerEndpoint)
        + "&key="
        + client.apiKey;

    std::string response;

    if (!httpPostJson(
            endpoint,
            body,
            client,
            response,
            error)) {
        return false;
    }

    try {
        std::istringstream input(response);

        ptree root;
        boost::property_tree::read_json(input, root);

        playerResponse = std::move(root);

        error.clear();
        return true;
    }
    catch (const std::exception& e) {
        error =
            std::string(
                "Could not parse YouTube's JSON response: ")
            + e.what();

        return false;
    }
}

// ============================================================
// Playlist parsing
// ============================================================

std::vector<std::string> parsePlaylistIdsFromHtml(
    const std::string& html)
{
    std::vector<std::string> ids;
    std::set<std::string> seen;

    // Search actual renderer objects first. The previous approach
    // could find a renderer and accidentally consume a videoId
    // belonging to a later, unrelated object.
    const std::vector<std::string> rendererNames = {
        "playlistVideoRenderer",
        "playlistPanelVideoRenderer",
        "gridVideoRenderer"
    };

    for (const std::string& renderer : rendererNames) {
        std::size_t position = 0;

        while ((position =
                    html.find(renderer, position))
               != std::string::npos) {
            const std::size_t afterName =
                position + renderer.size();

            const std::size_t colon =
                html.find(':', afterName);

            if (colon == std::string::npos) {
                break;
            }

            std::size_t objectStart = colon + 1;

            while (objectStart < html.size() &&
                   std::isspace(
                       static_cast<unsigned char>(
                           html[objectStart]))) {
                ++objectStart;
            }

            if (objectStart >= html.size() ||
                html[objectStart] != '{') {
                position = afterName;
                continue;
            }

            const std::size_t objectEnd =
                findJsonObjectEnd(html, objectStart);

            if (objectEnd == std::string::npos) {
                position = afterName;
                continue;
            }

            const std::string object =
                html.substr(
                    objectStart,
                    objectEnd - objectStart + 1);

            const std::string id =
                extractJsonStringField(
                    object,
                    "videoId");

            if (isValidVideoId(id) &&
                seen.insert(id).second) {
                ids.push_back(id);
            }

            position = objectEnd + 1;
        }
    }

    if (!ids.empty()) {
        return ids;
    }

    // Fallback for pages whose renderer layout has changed.
    // This is less precise because the page may also contain
    // recommended videos or other unrelated video IDs.
    const std::string marker = "\"videoId\"";
    std::size_t position = 0;

    while ((position =
                html.find(marker, position))
           != std::string::npos) {
        const std::string id =
            extractJsonStringField(
                html.substr(position),
                "videoId");

        if (isValidVideoId(id) &&
            seen.insert(id).second) {
            ids.push_back(id);
        }

        position += marker.size();
    }

    return ids;
}

} // namespace

// ============================================================
// Playlist functions
// ============================================================

bool fetchPlaylist(
    const std::string& url,
    PlaylistFetchResult& result)
{
    result = {};

    const std::string playlistId =
        extractPlaylistId(url);

    if (playlistId.empty()) {
        result.ok = false;
        result.error =
            "Could not extract a playlist ID from the URL.";

        return false;
    }

    std::string page;

    const std::string pageUrl =
        "https://www.youtube.com/playlist?list="
        + playlistId;

    if (!httpGetText(pageUrl, page, result.error)) {
        result.ok = false;
        return false;
    }

    const std::vector<std::string> ids =
        parsePlaylistIdsFromHtml(page);

    if (ids.empty()) {
        result.ok = false;
        result.error =
            "The playlist page did not expose any public "
            "video entries. It may be private, restricted, "
            "or have a changed page structure.";

        return false;
    }

    result.videoIds = ids;
    result.ok = true;
    result.error.clear();

    return true;
}

bool fetchPlaylistVideoIds(
    const std::string& url,
    std::vector<std::string>& videoIds,
    std::string& error)
{
    PlaylistFetchResult result;

    if (!fetchPlaylist(url, result)) {
        error = result.error;
        return false;
    }

    videoIds = std::move(result.videoIds);
    error.clear();

    return true;
}

// ============================================================
// Video metadata and format extraction
// ============================================================

bool fetchVideo(
    const std::string& url,
    VideoFetchResult& result)
{
    result = {};

    const std::string videoId =
        extractVideoId(url);

    if (videoId.empty()) {
        result.error =
            "Could not extract a video ID from the URL.";

        return false;
    }

    std::string lastStatus = "UNKNOWN";
    std::string lastReason =
        "YouTube did not return playback information.";

    std::string signInReason;
    std::string lastRequestError;

    bool sawSignInChallenge = false;
    bool receivedResponse = false;

    std::string bestTitle = "youtube-video";
    std::vector<Format> bestFormats;

    const std::vector<ClientProfile> clients =
        playerClients();

    for (const ClientProfile& client : clients) {
        ptree playerResponse;
        std::string requestError;

        if (!requestPlayerResponse(
                videoId,
                client,
                playerResponse,
                requestError)) {
            lastRequestError =
                client.name + ": " + requestError;

            continue;
        }

        receivedResponse = true;

        const std::string candidateTitle =
            playerResponse.get<std::string>(
                "videoDetails.title",
                "youtube-video");

        const std::string status =
            playerResponse.get<std::string>(
                "playabilityStatus.status",
                "UNKNOWN");

        std::string reason =
            playerResponse.get<std::string>(
                "playabilityStatus.reason",
                "");

        if (reason.empty()) {
            reason =
                playerResponse.get<std::string>(
                    "playabilityStatus.errorScreen."
                    "playerErrorMessageRenderer.reason.simpleText",
                    "No reason provided.");
        }

        std::vector<Format> candidateFormats =
            collectFormats(playerResponse);

        const bool hasDirectUrl =
            std::any_of(
                candidateFormats.begin(),
                candidateFormats.end(),
                [](const Format& format) {
                    return !format.url.empty();
                });

        // A client that returned directly usable URLs wins
        // immediately, avoiding unnecessary fallback requests.
        if (hasDirectUrl) {
            result.title = candidateTitle;
            result.formats = std::move(candidateFormats);
            result.ok = true;
            result.error.clear();

            return true;
        }

        // Keep the first nonempty format list as a fallback.
        // Formats without direct URLs may still be useful for
        // diagnostics or listing supported formats.
        if (bestFormats.empty() && !candidateFormats.empty()) {
            bestTitle = candidateTitle;
            bestFormats = std::move(candidateFormats);
        }

        const std::string lowerReason =
            lowercase(reason);

        const bool signInRequired =
            lowerReason.find("sign in") != std::string::npos
            || lowerReason.find("login required")
                != std::string::npos
            || lowerReason.find("verify") != std::string::npos;

        if (signInRequired) {
            sawSignInChallenge = true;
            signInReason = reason;
        }

        // Preserve a meaningful status and reason instead of
        // overwriting it with a later UNKNOWN response.
        if (status != "UNKNOWN" &&
            (!reason.empty() ||
             lastStatus == "UNKNOWN")) {
            lastStatus = status;

            if (!reason.empty()) {
                lastReason = reason;
            }
        }
    }

    result.title = bestTitle;
    result.formats = std::move(bestFormats);

    // Metadata may have been returned by a fallback client even
    // if its formats have no directly usable media URLs.
    // Let the caller list/report those formats.
    if (!result.formats.empty()) {
        result.ok = true;
        result.error.clear();

        return true;
    }

    result.ok = false;

    if (!receivedResponse) {
        result.error =
            lastRequestError.empty()
                ? "YouTube could not be reached."
                : lastRequestError;

        return false;
    }

    if (sawSignInChallenge && !signInReason.empty()) {
        result.error =
            "This video requires sign-in or a browser session: "
            + signInReason;
    }
    else if (lastStatus == "LOGIN_REQUIRED" ||
             lastStatus == "AGE_CHECK_REQUIRED" ||
             lastStatus == "AGE_VERIFICATION_REQUIRED") {
        result.error =
            "This video is restricted for this session: "
            + lastReason;
    }
    else {
        result.error =
            "YouTube did not provide playable formats for "
            "this video: "
            + lastReason;
    }

    return false;
}

bool fetchVideoInfo(
    const std::string& url,
    std::string& title,
    std::vector<Format>& formats)
{
    VideoFetchResult result;

    if (!fetchVideo(url, result)) {
        std::cerr
            << "Error: "
            << result.error
            << '\n';

        return false;
    }

    title = std::move(result.title);
    formats = std::move(result.formats);

    std::cout
        << "Video: "
        << title
        << "\n";

    return true;
}

} // namespace ytdl
