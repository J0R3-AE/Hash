#include "ytdl/utils.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace ytdl {

std::string trim(const std::string& value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};

    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string sanitizeFilename(std::string name)
{
    for (char& c : name) {
        const unsigned char ch = static_cast<unsigned char>(c);
        if (ch < 32 || c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') {
            c = '_';
        }
    }

    name = trim(name);
    while (!name.empty() && (name.back() == '.' || name.back() == ' '))
        name.pop_back();

    if (name.empty())
        name = "youtube-video";

    // Avoid excessively long path names while keeping ordinary titles intact.
    if (name.size() > 180)
        name.resize(180);

    return name;
}

static std::string takeIdFromPath(const std::string& url,
                           const std::string& marker)
{
    const auto pos = url.find(marker);
    if (pos == std::string::npos)
        return {};

    const std::size_t start = pos + marker.size();
    const auto end = url.find_first_of("/?&#", start);
    return url.substr(start, end == std::string::npos
                                ? std::string::npos
                                : end - start);
}

std::string extractVideoId(const std::string& input)
{
    const std::string url = trim(input);

    if (url.find("youtu.be/") != std::string::npos) {
        const std::string id = takeIdFromPath(url, "youtu.be/");
        if (!id.empty()) return id;
    }

    for (const std::string marker : {"/shorts/", "/embed/", "/live/"}) {
        const std::string id = takeIdFromPath(url, marker);
        if (!id.empty()) return id;
    }

    const auto query = url.find('?');
    if (query != std::string::npos) {
        std::size_t pos = query + 1;
        while (pos < url.size()) {
            const auto end = url.find('&', pos);
            const std::string part = url.substr(
                pos, end == std::string::npos ? std::string::npos : end - pos);
            if (part.rfind("v=", 0) == 0 && part.size() > 2)
                return part.substr(2);
            if (end == std::string::npos) break;
            pos = end + 1;
        }
    }

    // Also accept a bare video ID for convenience.
    if (url.find('/') == std::string::npos &&
        url.find('?') == std::string::npos &&
        url.find('&') == std::string::npos && !url.empty()) {
        return url;
    }

    return {};
}

std::string extractPlaylistId(const std::string& input)
{
    const std::string url = trim(input);
    if (url.empty())
        return {};

    const auto listPos = url.find("list=");
    if (listPos != std::string::npos) {
        const auto start = listPos + std::string("list=").size();
        const auto end = url.find_first_of("&/?#", start);
        const std::string id = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!id.empty())
            return id;
    }

    if (url.find("/playlist") != std::string::npos) {
        const auto marker = "/playlist";
        const auto pos = url.find(marker);
        if (pos != std::string::npos) {
            const auto start = url.find("list=", pos);
            if (start != std::string::npos) {
                const auto idStart = start + std::string("list=").size();
                const auto idEnd = url.find_first_of("&/?#", idStart);
                const std::string id = url.substr(idStart, idEnd == std::string::npos ? std::string::npos : idEnd - idStart);
                if (!id.empty())
                    return id;
            }
        }
    }

    return {};
}

bool isPlaylistUrl(const std::string& input)
{
    const std::string url = trim(input);
    if (url.empty())
        return false;
    return url.find("/playlist") != std::string::npos ||
           url.find("list=") != std::string::npos;
}

std::vector<std::string> parseFormatCodes(const std::string& input)
{
    std::vector<std::string> codes;
    std::size_t start = 0;

    while (start <= input.size()) {
        const auto comma = input.find(',', start);
        std::string code = trim(input.substr(
            start, comma == std::string::npos
                       ? std::string::npos : comma - start));
        if (!code.empty()) codes.push_back(code);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }

    return codes;
}

static bool looksLikeExtensionToken(const std::string& value)
{
    const std::string normalized = trim(value);
    if (normalized.empty())
        return false;

    const std::string lower = [] (const std::string& v) {
        std::string out = v;
        std::transform(out.begin(), out.end(), out.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return out;
    }(normalized);

    return lower == "mp4" || lower == "webm" || lower == "m4a" || lower == "mp3" ||
           lower == "aac" || lower == "ogg" || lower == "opus" || lower == "wav" ||
           lower == "flac";
}

static bool looksLikeQualityToken(const std::string& value)
{
    const std::string normalized = trim(value);
    if (normalized.empty())
        return false;

    if (normalized == "audio" || normalized == "audio-only")
        return true;

    const std::string lower = [] (const std::string& v) {
        std::string out = v;
        std::transform(out.begin(), out.end(), out.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return out;
    }(normalized);

    if (lower.size() > 1 && lower.back() == 'k') {
        const std::string digits = lower.substr(0, lower.size() - 1);
        return !digits.empty() && std::all_of(digits.begin(), digits.end(),
            [](unsigned char ch) { return std::isdigit(ch) != 0; });
    }

    if (lower.size() > 1 && lower.back() == 'p') {
        const std::string digits = lower.substr(0, lower.size() - 1);
        return !digits.empty() && std::all_of(digits.begin(), digits.end(),
            [](unsigned char ch) { return std::isdigit(ch) != 0; });
    }

    return std::all_of(normalized.begin(), normalized.end(),
        [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

std::vector<std::string> parseDownloadSelectors(const std::string& input)
{
    std::vector<std::string> parts;
    std::string current;
    for (char ch : input) {
        if (ch == ',') {
            const std::string token = trim(current);
            if (!token.empty())
                parts.push_back(token);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    const std::string token = trim(current);
    if (!token.empty())
        parts.push_back(token);

    std::vector<std::string> result;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const std::string& currentToken = parts[i];
        if (i + 1 < parts.size() && looksLikeExtensionToken(currentToken) &&
            looksLikeQualityToken(parts[i + 1])) {
            result.push_back(currentToken + ":" + parts[i + 1]);
            ++i;
            continue;
        }
        result.push_back(currentToken);
    }
    return result;
}

std::string readConfigValue(const std::string& configPath,
                           const std::string& key)
{
    std::ifstream input(configPath);
    if (!input)
        return {};

    const std::string normalizedKey = trim(key);
    std::string line;
    while (std::getline(input, line)) {
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';')
            continue;

        const auto equalsPos = trimmed.find('=');
        if (equalsPos == std::string::npos)
            continue;

        const std::string candidateKey = trim(trimmed.substr(0, equalsPos));
        if (candidateKey != normalizedKey)
            continue;

        std::string value = trim(trimmed.substr(equalsPos + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'')
            value = value.substr(1, value.size() - 2);
        return value;
    }

    return {};
}

std::string normalizeBrowserName(const std::string& input)
{
    std::string value = trim(input);
    if (value.empty())
        return "auto";

    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    if (value == "chrome" || value == "google-chrome")
        return "chrome";
    if (value == "edge" || value == "msedge" || value == "microsoft-edge")
        return "edge";
    if (value == "firefox" || value == "mozilla")
        return "firefox";
    if (value == "safari")
        return "safari";
    if (value == "brave")
        return "brave";
    return "auto";
}

std::string noCookiesWarningMessage()
{
    return "WARNING: no browser cookies or SQLite database were provided. "
           "Public videos may still work, but private, age-gated, and region-locked "
           "content can fail without a signed-in session. Re-run with --cookies <file> "
           "or --no-cookies to acknowledge this limitation explicitly.";
}

std::string browserUserAgentForName(const std::string& browser)
{
    const std::string normalized = normalizeBrowserName(browser);
    if (normalized == "edge") {
        return "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
               "(KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36 Edg/128.0.0.0";
    }
    if (normalized == "firefox") {
        return "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 "
               "Firefox/128.0";
    }
    if (normalized == "safari") {
        return "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 "
               "(KHTML, like Gecko) Version/18.0 Safari/605.1.15";
    }
    if (normalized == "brave") {
        return "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
               "Chrome/128.0.0.0 Safari/537.36 Brave/128.0.0.0";
    }
    if (normalized == "chrome") {
        return "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
               "(KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36";
    }

    return "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
           "(KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36";
}

bool isVideoRestrictedOrUnavailable(const std::string& status,
                                     const std::string& reason)
{
    const std::string normalizedStatus = trim(status);
    const std::string normalizedReason = trim(reason);
    if (normalizedStatus == "LOGIN_REQUIRED" ||
        normalizedStatus == "AGE_CHECK_REQUIRED" ||
        normalizedStatus == "UNAVAILABLE") {
        return true;
    }

    std::string lower = normalizedReason;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    return lower.find("sign in") != std::string::npos ||
           lower.find("not a bot") != std::string::npos ||
           lower.find("verify") != std::string::npos ||
           lower.find("unavailable") != std::string::npos ||
           lower.find("private video") != std::string::npos ||
           lower.find("removed") != std::string::npos ||
           lower.find("login required") != std::string::npos ||
           lower.find("age check") != std::string::npos ||
           lower.find("copyright") != std::string::npos ||
           lower.find("this video is unavailable") != std::string::npos ||
           lower.find("blocked") != std::string::npos;
}

std::vector<std::string> summarizeFormatEntries(
    const std::vector<Format>& formats)
{
    std::set<std::string> seen;
    std::vector<std::string> entries;

    for (const auto& format : formats) {
        const std::string resolution = trim(format.resolution);
        const std::string ext = trim(format.ext);
        if (resolution.empty() || resolution == "N/A" || ext.empty())
            continue;

        std::string label = resolution;
        if (label == "audio only")
            label = "audio-only";
        label += "/" + ext;

        if (seen.insert(label).second)
            entries.push_back(label);
    }

    std::sort(entries.begin(), entries.end(),
              [](const std::string& lhs, const std::string& rhs) {
                  const auto parse = [](const std::string& value) -> int {
                      const auto slash = value.find('/');
                      const std::string quality =
                          slash == std::string::npos ? value : value.substr(0, slash);
                      if (quality == "audio-only") return 1000000;
                      if (quality.empty()) return -1;
                      const auto end = quality.find_last_of("pP");
                      if (end == std::string::npos)
                          return 0;
                      const std::string digits = quality.substr(0, end);
                      if (digits.empty())
                          return 0;
                      try {
                          return std::stoi(digits);
                      } catch (const std::exception&) {
                          return 0;
                      }
                  };
                  const auto lhsValue = parse(lhs);
                  const auto rhsValue = parse(rhs);
                  if (lhsValue != rhsValue)
                      return lhsValue < rhsValue;
                  return lhs < rhs;
              });

    return entries;
}

std::vector<std::string> summarizeFormatResolutions(
    const std::vector<Format>& formats)
{
    std::set<std::string> seen;
    std::vector<std::string> resolutions;

    for (const auto& format : formats) {
        const std::string resolution = trim(format.resolution);
        if (resolution.empty() || resolution == "N/A")
            continue;
        if (seen.insert(resolution).second)
            resolutions.push_back(resolution);
    }

    std::sort(resolutions.begin(), resolutions.end(),
              [](const std::string& lhs, const std::string& rhs) {
                  const auto parse = [](const std::string& value) -> int {
                      if (value == "audio only") return 1000000;
                      if (value.empty() || value == "N/A") return -1;
                      const auto end = value.find_last_of("pP");
                      if (end == std::string::npos)
                          return 0;
                      const std::string digits = value.substr(0, end);
                      if (digits.empty())
                          return 0;
                      try {
                          return std::stoi(digits);
                      } catch (const std::exception&) {
                          return 0;
                      }
                  };
                  return parse(lhs) < parse(rhs);
              });

    return resolutions;
}

} // namespace ytdl
