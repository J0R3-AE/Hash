#include "parser.hpp"

#include "ytdl/utils.hpp"

#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

using boost::property_tree::ptree;

namespace ytdl {
namespace {

std::string getField(const ptree& object, const std::string& key,
                     const std::string& fallback = "N/A")
{
    const auto value = object.get_optional<std::string>(key);
    if (!value || value->empty() || *value == "null")
        return fallback;
    return *value;
}

std::string firstField(const ptree& object,
                       const std::vector<std::string>& keys,
                       const std::string& fallback = "N/A")
{
    for (const auto& key : keys) {
        const std::string value = getField(object, key, "");
        if (!value.empty())
            return value;
    }
    return fallback;
}

std::string parseCodecs(const std::string& mimeType)
{
    const std::string marker = "codecs=\"";
    const auto start = mimeType.find(marker);
    if (start == std::string::npos)
        return {};

    const std::size_t valueStart = start + marker.size();
    const auto end = mimeType.find('"', valueStart);
    if (end == std::string::npos)
        return {};

    return mimeType.substr(valueStart, end - valueStart);
}

std::vector<std::string> splitCodecs(const std::string& codecs)
{
    std::vector<std::string> result;
    std::size_t start = 0;

    while (start <= codecs.size()) {
        const auto comma = codecs.find(',', start);
        std::string value = trim(codecs.substr(
            start, comma == std::string::npos
                       ? std::string::npos : comma - start));
        if (!value.empty()) result.push_back(value);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }

    return result;
}

std::string calculateBitrateKbps(const ptree& format)
{
    const std::string raw = firstField(
        format, {"averageBitrate", "bitrate"}, "");
    if (raw.empty()) return "N/A";

    try {
        const double kbps = std::stod(raw) / 1000.0;
        std::ostringstream out;
        out << std::fixed << std::setprecision(1) << kbps;
        return out.str();
    } catch (...) {
        return "N/A";
    }
}

Format parseFormat(const ptree& raw)
{
    Format format;
    format.id = firstField(raw, {"itag", "format_id"}, "");
    format.ext = firstField(raw, {"ext"}, "");
    format.url = firstField(raw, {"url"}, "");
    format.fps = firstField(raw, {"fps"}, "N/A");
    format.note = firstField(raw, {"format_note", "qualityLabel", "quality"}, "");
    format.bitrateKbps = calculateBitrateKbps(raw);

    const std::string mimeType = getField(raw, "mimeType", "");
    if (format.ext.empty()) {
        const auto slash = mimeType.find('/');
        const auto semicolon = mimeType.find(';');
        if (slash != std::string::npos) {
            const std::size_t start = slash + 1;
            format.ext = mimeType.substr(
                start, semicolon == std::string::npos
                           ? std::string::npos : semicolon - start);
        }
    }
    if (format.ext.empty()) format.ext = "bin";

    const std::string width = getField(raw, "width", "");
    const std::string height = getField(raw, "height", "");
    format.resolution = firstField(raw, {"resolution", "qualityLabel"}, "");
    if (format.resolution.empty() && !width.empty() && !height.empty())
        format.resolution = width + "x" + height;

    const std::string mimeLower = [&]() {
        std::string value = mimeType;
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }();

    const auto codecs = splitCodecs(parseCodecs(mimeType));
    if (mimeLower.rfind("audio/", 0) == 0) {
        format.videoCodec = "none";
        format.audioCodec = codecs.empty() ? "N/A" : codecs.front();
        if (format.resolution.empty()) format.resolution = "audio only";
    } else if (mimeLower.rfind("video/", 0) == 0) {
        format.videoCodec = codecs.empty() ? "N/A" : codecs.front();
        format.audioCodec = codecs.size() > 1 ? codecs[1] : "none";
        if (format.resolution.empty()) format.resolution = "N/A";
    } else {
        format.videoCodec = getField(raw, "vcodec", "N/A");
        format.audioCodec = getField(raw, "acodec", "N/A");
        if (format.resolution.empty()) format.resolution = "N/A";
    }

    format.videoCodec = getField(raw, "vcodec", format.videoCodec);
    format.audioCodec = getField(raw, "acodec", format.audioCodec);
    return format;
}

} // namespace

std::vector<Format> collectFormats(const ptree& playerResponse)
{
    std::vector<Format> formats;
    std::unordered_set<std::string> seenIds;

    const auto streaming = playerResponse.get_child_optional("streamingData");
    if (!streaming) return formats;

    for (const std::string arrayName : {"formats", "adaptiveFormats"}) {
        const auto list = streaming->get_child_optional(arrayName);
        if (!list) continue;

        for (const auto& entry : *list) {
            Format format = parseFormat(entry.second);
            if (format.id.empty() || !seenIds.insert(format.id).second)
                continue;
            formats.push_back(std::move(format));
        }
    }

    return formats;
}

} // namespace ytdl
