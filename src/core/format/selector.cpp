#include "ytdl/format_selector.hpp"

#include "ytdl/utils.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <tuple>
#include <vector>

namespace ytdl {
namespace {

std::string lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string normalizeExtension(std::string value)
{
    value = lowercase(trim(value));
    if (!value.empty() && value.front() == '.')
        value.erase(value.begin());
    return value;
}

bool isNumericId(const std::string& value)
{
    return !value.empty() && std::all_of(value.begin(), value.end(),
        [](unsigned char c) { return std::isdigit(c) != 0; });
}

bool hasCodec(const std::string& codec)
{
    const std::string normalized = lowercase(trim(codec));
    return !normalized.empty() && normalized != "none" &&
           normalized != "n/a" && normalized != "null";
}

bool hasVideo(const Format& format)
{
    return hasCodec(format.videoCodec);
}

bool hasAudio(const Format& format)
{
    return hasCodec(format.audioCodec);
}

bool isVideoContainer(const std::string& ext)
{
    return ext == "mp4" || ext == "webm" || ext == "mov" ||
           ext == "3gp" || ext == "mkv";
}

std::pair<std::string, std::string> splitSelectorSpec(const std::string& selector)
{
    const auto colon = selector.find(':');
    if (colon == std::string::npos)
        return {normalizeExtension(selector), {}};
    return {normalizeExtension(selector.substr(0, colon)),
            trim(selector.substr(colon + 1))};
}

long long resolutionRank(const std::string& resolution)
{
    const auto x = resolution.find('x');
    std::size_t start = x == std::string::npos ? 0 : x + 1;
    while (start < resolution.size() &&
           !std::isdigit(static_cast<unsigned char>(resolution[start]))) {
        ++start;
    }

    if (start == resolution.size())
        return 0;

    try {
        return std::stoll(resolution.substr(start));
    } catch (...) {
        return 0;
    }
}

double bitrateRank(const std::string& bitrate)
{
    try {
        return std::stod(bitrate);
    } catch (...) {
        return 0.0;
    }
}

int mediaRank(const Format& format)
{
    if (hasVideo(format) && hasAudio(format)) return 3;
    if (hasVideo(format)) return 2;
    if (hasAudio(format)) return 1;
    return 0;
}

FormatSelection resolveExtension(
    const std::string& selector,
    const std::vector<Format>& formats,
    const std::set<std::string>& alreadySelectedIds)
{
    const auto [wantedExt, wantedQuality] = splitSelectorSpec(selector);
    FormatSelection result;
    if (wantedExt.empty()) {
        result.error = "Empty format selector.";
        return result;
    }

    bool extensionExists = false;
    bool anyDirectUrl = false;
    bool anyMuxedVideo = false;
    std::vector<const Format*> candidates;

    for (const auto& format : formats) {
        if (normalizeExtension(format.ext) != wantedExt)
            continue;

        extensionExists = true;
        if (format.url.empty())
            continue;

        anyDirectUrl = true;
        if (isVideoContainer(wantedExt) && hasVideo(format) && hasAudio(format))
            anyMuxedVideo = true;

        if (!wantedQuality.empty()) {
            if (alreadySelectedIds.count(format.id) != 0)
                continue;
            candidates.push_back(&format);
            continue;
        }

        if (isVideoContainer(wantedExt)) {
            if (!hasVideo(format) || !hasAudio(format))
                continue;
        }

        if (alreadySelectedIds.count(format.id) != 0)
            continue;

        candidates.push_back(&format);
    }

    if (candidates.empty()) {
        if (!extensionExists) {
            if (wantedExt == "mp3") {
                result.error =
                    "YouTube does not normally provide MP3 streams, and this "
                    "native downloader does not transcode audio. Choose an "
                    "available format such as m4a, or add an audio-conversion step.";
            } else {
                result.error = "YouTube did not return any '" + wantedExt +
                    "' formats for this client. Run 'list' to see available extensions.";
            }
        } else if (!anyDirectUrl) {
            result.error = "YouTube returned '" + wantedExt +
                "' formats, but none has a usable direct URL; signature deciphering "
                "or additional playback authorization may be required.";
        } else if (isVideoContainer(wantedExt) && !anyMuxedVideo) {
            result.error = "The available '" + wantedExt +
                "' streams are video-only or audio-only. This downloader cannot "
                "merge separate streams yet.";
        } else {
            result.error = "No additional directly downloadable '" + wantedExt +
                "' format remains. Repeating an extension selects distinct formats; "
                "use a numeric ID to request a specific format.";
        }
        return result;
    }

    if (!wantedQuality.empty()) {
        const auto normalizedQuality = lowercase(trim(wantedQuality));
        const bool isResolution = !normalizedQuality.empty() &&
            (normalizedQuality.back() == 'p' ||
             std::all_of(normalizedQuality.begin(), normalizedQuality.end(),
                         [](unsigned char ch) { return std::isdigit(ch) != 0; }));
        const bool isBitrate = !normalizedQuality.empty() &&
            normalizedQuality.back() == 'k';
        const auto parseTarget = [](const std::string& value) -> long long {
            const std::string normalized = lowercase(trim(value));
            if (normalized.empty()) return 0;
            if (normalized == "audio-only" || normalized == "audio only") return 0;
            const bool isBits = normalized.size() > 1 && normalized.back() == 'k';
            const bool isPixels = normalized.size() > 1 && normalized.back() == 'p';
            const std::string digits = (isBits || isPixels) ? normalized.substr(0, normalized.size() - 1) : normalized;
            if (digits.empty())
                return 0;
            try {
                return std::stoll(digits);
            } catch (...) {
                return 0;
            }
        };

        const long long target = parseTarget(normalizedQuality);
        std::vector<const Format*> filtered;
        for (const Format* candidate : candidates) {
            if (isResolution && (lowercase(trim(candidate->resolution)) == "audio only" ||
                                 lowercase(trim(candidate->resolution)) == "audio-only"))
                continue;
            if (isBitrate && bitrateRank(candidate->bitrateKbps) <= 0.0)
                continue;
            filtered.push_back(candidate);
        }

        if (filtered.empty()) {
            result.error = "No '" + wantedExt + "' format matches quality '" + wantedQuality + "'.";
            return result;
        }

        std::stable_sort(filtered.begin(), filtered.end(),
            [target, isResolution, isBitrate](const Format* a, const Format* b) {
                const auto valueOf = [&](const Format* f) -> long long {
                    if (isBitrate)
                        return static_cast<long long>(bitrateRank(f->bitrateKbps));
                    if (isResolution)
                        return resolutionRank(f->resolution);
                    return 0;
                };
                const long long diffA = std::llabs(target - valueOf(a));
                const long long diffB = std::llabs(target - valueOf(b));
                if (diffA != diffB)
                    return diffA < diffB;
                return std::tuple<int, long long, double>(
                           mediaRank(*a), resolutionRank(a->resolution), bitrateRank(a->bitrateKbps)) >
                       std::tuple<int, long long, double>(
                           mediaRank(*b), resolutionRank(b->resolution), bitrateRank(b->bitrateKbps));
            });
        result.format = filtered.front();
        return result;
    }

    std::stable_sort(candidates.begin(), candidates.end(),
        [](const Format* a, const Format* b) {
            return std::tuple<int, long long, double>(
                       mediaRank(*a), resolutionRank(a->resolution), bitrateRank(a->bitrateKbps)) >
                   std::tuple<int, long long, double>(
                       mediaRank(*b), resolutionRank(b->resolution), bitrateRank(b->bitrateKbps));
        });

    result.format = candidates.front();
    return result;
}

} // namespace

FormatSelection resolveFormatSelector(
    const std::string& selector,
    const std::vector<Format>& formats,
    const std::set<std::string>& alreadySelectedIds)
{
    if (isNumericId(selector)) {
        const auto found = std::find_if(
            formats.begin(), formats.end(),
            [&](const Format& format) { return format.id == selector; });

        if (found == formats.end())
            return {nullptr, "Format code " + selector + " was not returned by YouTube."};

        return {&*found, {}};
    }

    return resolveExtension(selector, formats, alreadySelectedIds);
}

} // namespace ytdl
