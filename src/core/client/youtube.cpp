#include "ytdl/youtube_client.hpp"

#include "../format/parser.hpp"
#include "ytdl/http_client.hpp"
#include "ytdl/utils.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using boost::property_tree::ptree;

namespace ytdl {
namespace {

constexpr const char* kPlayerEndpoint =
    "https://www.youtube.com/youtubei/v1/player?prettyPrint=false";

std::string jsonQuote(const std::string& value)
{
    std::string result = "\"";
    for (unsigned char c : value) {
        switch (c) {
        case '\"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (c < 0x20) {
                static constexpr char hex[] = "0123456789abcdef";
                result += "\\u00";
                result += hex[(c >> 4) & 0x0f];
                result += hex[c & 0x0f];
            } else {
                result += static_cast<char>(c);
            }
        }
    }
    result += '\"';
    return result;
}

std::string extractConfigString(const std::string& html, const std::string& key)
{
    const std::string marker = "\"" + key + "\"";
    const std::size_t markerPos = html.find(marker);
    if (markerPos == std::string::npos)
        return {};

    const std::size_t colon = html.find(':', markerPos + marker.size());
    if (colon == std::string::npos)
        return {};

    std::size_t valuePos = colon + 1;
    while (valuePos < html.size() &&
           std::isspace(static_cast<unsigned char>(html[valuePos])))
        ++valuePos;
    if (valuePos >= html.size() || html[valuePos] != '"')
        return {};

    ++valuePos;
    std::string value;
    bool escaped = false;
    for (std::size_t i = valuePos; i < html.size(); ++i) {
        const char c = html[i];
        if (escaped) {
            value.push_back(c);
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            return value;
        } else {
            value.push_back(c);
        }
    }
    return {};
}

std::vector<ClientProfile> playerClients()
{
    std::vector<ClientProfile> clients;
    const std::string webUserAgent = browserUserAgentForName(getBrowserProfileName());

    std::string homepage;
    std::string homepageError;
    if (httpGetText("https://www.youtube.com/", homepage, homepageError)) {
        const std::string apiKey = extractConfigString(homepage, "INNERTUBE_API_KEY");
        const std::string clientVersion = extractConfigString(homepage, "INNERTUBE_CLIENT_VERSION");
        if (!apiKey.empty() && !clientVersion.empty()) {
            clients.push_back({
                "WEB", clientVersion, "1", apiKey, webUserAgent,
                "\"clientScreen\":\"WATCH\","
                "\"platform\":\"DESKTOP\""
            });
        } else {
            std::cerr << "Warning: could not find the current WEB client configuration; "
                         "trying fallback player profiles.\n";
        }
    } else {
        std::cerr << "Warning: could not fetch YouTube's homepage to discover the WEB "
                     "client configuration (" << homepageError << "); trying fallback "
                     "player profiles.\n";
    }

    clients.push_back({
        "VISIONOS", "1.02", "101",
        "AIzaSyB-63vPrdThhKuerbB2N_l7Kwwcxj6yUAc",
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) "
        "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15",
        "\"deviceMake\":\"Apple\","
        "\"deviceModel\":\"RealityDevice17,1\","
        "\"osName\":\"visionOS\","
        "\"osVersion\":\"26.5.23O471\""
    });
    clients.push_back({
        "IOS", "21.26.4", "5",
        "AIzaSyB-63vPrdThhKuerbB2N_l7Kwwcxj6yUAc",
        "com.google.ios.youtube/21.26.4 (iPhone16,2; U; CPU iOS 18_3_2 like Mac OS X;)",
        "\"deviceMake\":\"Apple\","
        "\"deviceModel\":\"iPhone16,2\","
        "\"osName\":\"iPhone\","
        "\"osVersion\":\"18.3.2.22D82\""
    });
    clients.push_back({
        "ANDROID", "21.26.364", "3",
        "AIzaSyA8eiZmM1FaDVjRy-df2KTyQ_vz_yYM39w",
        "com.google.android.youtube/21.26.364 (Linux; U; Android 11) gzip",
        "\"androidSdkVersion\":30,"
        "\"osName\":\"Android\","
        "\"osVersion\":\"11\""
    });
    return clients;
}

bool requestPlayerResponse(const std::string& videoId,
                           const ClientProfile& client,
                           ptree& playerResponse,
                           std::string& error)
{
    const std::string body =
        "{\"context\":{\"client\":{"
        "\"clientName\":" + jsonQuote(client.name) + ","
        "\"clientVersion\":" + jsonQuote(client.version) + ","
        "\"userAgent\":" + jsonQuote(client.userAgent) + "," +
        client.extraClientFields + ","
        "\"hl\":\"en\",\"gl\":\"US\"}},"
        "\"videoId\":" + jsonQuote(videoId) + ","
        "\"contentCheckOk\":true,\"racyCheckOk\":true}";

    const std::string endpoint = std::string(kPlayerEndpoint) +
        "&key=" + client.apiKey;
    std::string response;
    if (!httpPostJson(endpoint, body, client, response, error))
        return false;

    try {
        std::istringstream input(response);
        ptree root;
        boost::property_tree::read_json(input, root);
        playerResponse = root;
        return true;
    } catch (const std::exception& e) {
        error = std::string("Could not parse YouTube's JSON response: ") + e.what();
        return false;
    }
}

std::vector<std::string> parsePlaylistIdsFromHtml(const std::string& html)
{
    std::vector<std::string> ids;
    std::set<std::string> seen;

    const std::vector<std::string> rendererNames = {
        "playlistVideoRenderer",
        "playlistPanelVideoRenderer",
        "gridVideoRenderer"
    };

    for (const std::string& renderer : rendererNames) {
        std::size_t pos = 0;
        while ((pos = html.find(renderer, pos)) != std::string::npos) {
            const std::size_t start = html.find("\"videoId\":\"", pos);
            if (start == std::string::npos) {
                pos += renderer.size();
                continue;
            }

            const std::size_t valueStart = start + std::string("\"videoId\":\"").size();
            const std::size_t valueEnd = html.find('"', valueStart);
            if (valueEnd == std::string::npos) {
                pos += renderer.size();
                continue;
            }

            const std::string id = html.substr(valueStart, valueEnd - valueStart);
            if (!id.empty() && id.size() == 11 && seen.insert(id).second)
                ids.push_back(id);

            pos = valueEnd + 1;
        }
    }

    if (!ids.empty())
        return ids;

    const std::string marker = "\"videoId\":\"";
    std::size_t pos = 0;
    while ((pos = html.find(marker, pos)) != std::string::npos) {
        const std::size_t start = pos + marker.size();
        const std::size_t end = html.find('"', start);
        if (end == std::string::npos)
            break;

        const std::string id = html.substr(start, end - start);
        if (!id.empty() && id.size() == 11 && seen.insert(id).second)
            ids.push_back(id);

        pos = end + 1;
    }

    return ids;
}

} // namespace

bool fetchPlaylist(const std::string& url, PlaylistFetchResult& result)
{
    const std::string playlistId = extractPlaylistId(url);
    if (playlistId.empty()) {
        result.ok = false;
        result.error = "Could not extract a playlist ID from the URL.";
        return false;
    }

    std::string page;
    const std::string pageUrl = "https://www.youtube.com/playlist?list=" + playlistId;
    if (!httpGetText(pageUrl, page, result.error)) {
        result.ok = false;
        return false;
    }

    const std::vector<std::string> ids = parsePlaylistIdsFromHtml(page);
    if (ids.empty()) {
        result.ok = false;
        result.error = "The playlist page did not expose any public video entries.";
        return false;
    }

    result.videoIds = ids;
    result.ok = true;
    result.error.clear();
    return true;
}

bool fetchPlaylistVideoIds(const std::string& url,
                          std::vector<std::string>& videoIds,
                          std::string& error)
{
    PlaylistFetchResult result;
    if (!fetchPlaylist(url, result)) {
        error = result.error;
        return false;
    }

    videoIds = result.videoIds;
    error.clear();
    return true;
}

bool fetchVideo(const std::string& url, VideoFetchResult& result)
{
    result = {};
    const std::string videoId = extractVideoId(url);
    if (videoId.empty()) {
        result.error = "Could not extract a video ID from the URL.";
        return false;
    }

    std::string lastStatus = "UNKNOWN";
    std::string lastReason = "YouTube did not return playback information.";
    std::string signInReason;
    std::string lastRequestError;
    bool sawSignInChallenge = false;
    std::string bestTitle = "youtube-video";
    std::vector<Format> bestFormats;
    bool receivedResponse = false;

    const std::vector<ClientProfile> clients = playerClients();
    for (const ClientProfile& client : clients) {
        ptree playerResponse;
        std::string error;
        if (!requestPlayerResponse(videoId, client, playerResponse, error)) {
            lastRequestError = client.name + std::string(": ") + error;
            continue;
        }

        receivedResponse = true;
        const std::string candidateTitle = playerResponse.get<std::string>(
            "videoDetails.title", "youtube-video");
        const std::string status = playerResponse.get<std::string>(
            "playabilityStatus.status", "UNKNOWN");
        const std::string reason = playerResponse.get<std::string>(
            "playabilityStatus.reason", "No reason provided.");
        std::vector<Format> candidateFormats = collectFormats(playerResponse);

        if (!candidateFormats.empty()) {
            const bool hasDirectUrl = std::any_of(
                candidateFormats.begin(), candidateFormats.end(),
                [](const Format& format) { return !format.url.empty(); });

            if (hasDirectUrl || bestFormats.empty()) {
                bestTitle = candidateTitle;
                bestFormats = std::move(candidateFormats);
            }
            if (hasDirectUrl) {
                result.title = bestTitle;
                result.formats = std::move(bestFormats);
                result.ok = true;
                result.error.clear();
                return true;
            }
        }

        lastStatus = status;
        lastReason = reason;
        std::string lowerReason = reason;
        std::transform(lowerReason.begin(), lowerReason.end(), lowerReason.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (lowerReason.find("sign in") != std::string::npos ||
            lowerReason.find("login required") != std::string::npos ||
            lowerReason.find("verify") != std::string::npos) {
            sawSignInChallenge = true;
            signInReason = reason;
        }

        if (status == "ERROR" || status == "LOGIN_REQUIRED" ||
            status == "AGE_CHECK_REQUIRED" || status == "UNAVAILABLE") {
            if (lowerReason.find("sign in") != std::string::npos ||
                lowerReason.find("login required") != std::string::npos ||
                lowerReason.find("verify") != std::string::npos) {
                sawSignInChallenge = true;
                signInReason = reason;
            }
        }
    }

    if (!receivedResponse) {
        result.error = lastRequestError.empty() ? "YouTube could not be reached." : lastRequestError;
        return false;
    }

    result.title = bestTitle;
    result.formats = std::move(bestFormats);
    result.ok = false;

    if (sawSignInChallenge && !signInReason.empty()) {
        result.error = "This video requires sign-in or a browser session: " + signInReason;
    } else if (lastStatus == "LOGIN_REQUIRED" || lastStatus == "AGE_CHECK_REQUIRED") {
        result.error = "This video is restricted for this anonymous session: " + lastReason;
    } else if (!result.formats.empty()) {
        result.ok = true;
        result.error.clear();
    } else {
        result.error = "YouTube did not provide playable formats for this video: " + lastReason;
    }

    return result.ok;
}

bool fetchVideoInfo(const std::string& url, std::string& title,
                    std::vector<Format>& formats)
{
    VideoFetchResult result;
    if (!fetchVideo(url, result)) {
        std::cerr << "Error: " << result.error << '\n';
        return false;
    }

    title = result.title;
    formats = std::move(result.formats);
    std::cout << "Video: " << title << "\n";
    return true;
}

} // namespace ytdl
