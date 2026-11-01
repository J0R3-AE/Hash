
#include "ytdl/format_selector.hpp"

#include "ytdl/utils.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <tuple>
#include <vector>

namespace ytdl {
namespace {

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

std::string normalizeExtension(std::string value)
{
    value = lowercase(trim(value));

    if (!value.empty() && value.front() == '.') {
        value.erase(value.begin());
    }

    return value;
}

bool isNumericId(const std::string& value)
{
    return !value.empty() &&
        std::all_of(
            value.begin(),
            value.end(),
            [](unsigned char c) {
                return std::isdigit(c) != 0;
            });
}

bool hasCodec(const std::string& codec)
{
    const std::string normalized = lowercase(trim(codec));

    return !normalized.empty() &&
        normalized != "none" &&
        normalized != "n/a" &&
        normalized != "null";
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
    return ext == "mp4" || ext == "webm" ||
        ext == "mov" || ext == "3gp" ||
        ext == "mkv";
}

std::pair<std::string, std::string> splitSelectorSpec(
    const std::string& selector)
{
    const auto colon = selector.find(':');

    if (colon == std::string::npos) {
        return {normalizeExtension(selector), {}};
    }

    return {
        normalizeExtension(selector.substr(0, colon)),
        trim(selector.substr(colon + 1))
    };
}

long long resolutionRank(const std::string& resolution)
{
    const auto x = resolution.find('x');

    std::size_t start =
        x == std::string::npos ? 0 : x + 1;

    while (start < resolution.size() &&
           !std::isdigit(
               static_cast<unsigned char>(resolution[start]))) {
        ++start;
    }

    if (start == resolution.size()) {
        return 0;
    }

    try {
        return std::stoll(resolution.substr(start));
    }
    catch (...) {
        return 0;
    }
}

double bitrateRank(const std::string& bitrate)
{
    try {
        return std::stod(bitrate);
    }
    catch (...) {
        return 0.0;
    }
}

int mediaRank(const Format& format)
{
    if (hasVideo(format) && hasAudio(format)) {
        return 3;
    }

    if (hasVideo(format)) {
        return 2;
    }

    if (hasAudio(format)) {
        return 1;
    }

    return 0;
}

bool isResolutionQuality(const std::string& quality)
{
    const std::string value = lowercase(trim(quality));

    if (!value.empty() && value.back() == 'p') {
        return value.size() > 1 &&
            std::all_of(
                value.begin(),
                value.end() - 1,
                [](unsigned char c) {
                    return std::isdigit(c) != 0;
                });
    }

    return !value.empty() &&
        std::all_of(
            value.begin(),
            value.end(),
            [](unsigned char c) {
                return std::isdigit(c) != 0;
            });
}

bool isBitrateQuality(const std::string& quality)
{
    const std::string value = lowercase(trim(quality));

    return value.size() > 1 && value.back() == 'k';
}

long long parseQualityTarget(const std::string& quality)
{
    std::string value = lowercase(trim(quality));

    if (value.empty()) {
        return 0;
    }

    if (value.back() == 'p' || value.back() == 'k') {
        value.pop_back();
    }

    if (value.empty() ||
        !std::all_of(
            value.begin(),
            value.end(),
            [](unsigned char c) {
                return std::isdigit(c) != 0;
            })) {
        return 0;
    }

    try {
        return std::stoll(value);
    }
    catch (...) {
        return 0;
    }
}

FormatSelection resolveExtension(
    const std::string& selector,
    const std::vector<Format>& formats,
    const std::set<std::string>& alreadySelectedIds)
{
    const auto [wantedExt, wantedQuality] =
        splitSelectorSpec(selector);

    FormatSelection result;

    if (wantedExt.empty()) {
        result.error = "Empty format selector.";
        return result;
    }

    const bool hasQuality = !wantedQuality.empty();
    const bool isResolution = isResolutionQuality(wantedQuality);
    const bool isBitrate = isBitrateQuality(wantedQuality);
    const long long target = parseQualityTarget(wantedQuality);

    bool extensionExists = false;
    bool anyDirectUrl = false;
    bool anyMuxedVideo = false;

    // Track whether the requested resolution exists in YouTube's
    // metadata, even if it doesn't have a usable direct URL.
    bool requestedResolutionFound = false;
    bool requestedResolutionUsable = false;

    std::vector<const Format*> candidates;

    for (const auto& format : formats) {
        if (normalizeExtension(format.ext) != wantedExt) {
            continue;
        }

        extensionExists = true;

        const bool muxed =
            hasVideo(format) && hasAudio(format);

        const long long rank =
            resolutionRank(format.resolution);

        if (hasQuality && isResolution &&
            target > 0 && rank == target) {
            requestedResolutionFound = true;

            if (!format.url.empty() &&
                (!isVideoContainer(wantedExt) || muxed)) {
                requestedResolutionUsable = true;
            }
        }

        if (format.url.empty()) {
            continue;
        }

        anyDirectUrl = true;

        if (isVideoContainer(wantedExt)) {
            if (muxed) {
                anyMuxedVideo = true;
            }

            // This downloader does not yet merge separate video
            // and audio tracks, so don't select either track alone.
            if (!muxed) {
                continue;
            }
        }

        if (alreadySelectedIds.count(format.id) != 0) {
            continue;
        }

        candidates.push_back(&format);
    }

    // Never silently claim to fulfil an exact advertised resolution
    // by selecting a lower-resolution or audio-only stream.
    if (isResolution &&
        target > 0 &&
        requestedResolutionFound &&
        !requestedResolutionUsable) {
        result.error =
            "YouTube advertises the requested resolution '" +
            wantedQuality + "' for '" + wantedExt +
            "', but no directly downloadable stream containing " 
            "both audio and video is available at that resolution. "
            "The format may require signature deciphering or separate "
            "audio/video merging, which this downloader does not "
            "currently implement. No lower-resolution fallback was selected.";

        return result;
    }

    if (candidates.empty()) {
        if (!extensionExists) {
            if (wantedExt == "mp3") {
                result.error =
                    "YouTube does not normally provide MP3 streams, "
                    "and this native downloader does not transcode audio. "
                    "Choose an available format such as m4a, or add "
                    "an audio-conversion step.";
            }
            else {
                result.error =
                    "YouTube did not return any '" + wantedExt +
                    "' formats for this client. Run 'list' to see "
                    "available extensions.";
            }
        }
        else if (!anyDirectUrl) {
            result.error =
                "YouTube returned '" + wantedExt +
                "' formats, but none has a usable direct URL. "
                "Signature deciphering or additional playback "
                "authorization may be required.";
        }
        else if (isVideoContainer(wantedExt) && !anyMuxedVideo) {
            result.error =
                "The available '" + wantedExt +
                "' streams are separate video-only or audio-only "
                "tracks. This downloader cannot merge separate streams yet.";
        }
        else {
            result.error =
                "No additional directly downloadable '" + wantedExt +
                "' format remains. Repeating an extension selects "
                "distinct formats; use a numeric ID for a specific format.";
        }

        return result;
    }

    if (hasQuality) {
        std::vector<const Format*> filtered;

        for (const Format* candidate : candidates) {
            const std::string resolution =
                lowercase(trim(candidate->resolution));

            if (isResolution &&
                (resolution == "audio only" ||
                 resolution == "audio-only")) {
                continue;
            }

            if (isBitrate &&
                bitrateRank(candidate->bitrateKbps) <= 0.0) {
                continue;
            }

            filtered.push_back(candidate);
        }

        if (filtered.empty()) {
            result.error =
                "No '" + wantedExt +
                "' format matches quality '" + wantedQuality + "'.";
            return result;
        }

        std::stable_sort(
            filtered.begin(),
            filtered.end(),
            [target, isResolution, isBitrate](
                const Format* a,
                const Format* b) {
                const long long resolutionA =
                    resolutionRank(a->resolution);

                const long long resolutionB =
                    resolutionRank(b->resolution);

                if (isResolution && target > 0 &&
                    resolutionA != resolutionB) {
                    const bool aWithin = resolutionA <= target;
                    const bool bWithin = resolutionB <= target;

                    // Prefer resolutions at or below the requested
                    // maximum, choosing the highest such resolution.
                    if (aWithin != bWithin) {
                        return aWithin;
                    }

                    if (aWithin) {
                        return resolutionA > resolutionB;
                    }

                    // If nothing is at or below the requested size,
                    // prefer the smallest resolution above it.
                    return resolutionA < resolutionB;
                }

                if (isBitrate) {
                    const double bitrateA =
                        bitrateRank(a->bitrateKbps);

                    const double bitrateB =
                        bitrateRank(b->bitrateKbps);

                    const double differenceA =
                        std::abs(static_cast<double>(target) - bitrateA);

                    const double differenceB =
                        std::abs(static_cast<double>(target) - bitrateB);

                    if (differenceA != differenceB) {
                        return differenceA < differenceB;
                    }
                }

                return std::tuple<int, long long, double>(
                           mediaRank(*a),
                           resolutionRank(a->resolution),
                           bitrateRank(a->bitrateKbps)) >
                       std::tuple<int, long long, double>(
                           mediaRank(*b),
                           resolutionRank(b->resolution),
                           bitrateRank(b->bitrateKbps));
            });

        result.format = filtered.front();
        return result;
    }

    std::stable_sort(
        candidates.begin(),
        candidates.end(),
        [](const Format* a, const Format* b) {
            return std::tuple<int, long long, double>(
                       mediaRank(*a),
                       resolutionRank(a->resolution),
                       bitrateRank(a->bitrateKbps)) >
                   std::tuple<int, long long, double>(
                       mediaRank(*b),
                       resolutionRank(b->resolution),
                       bitrateRank(b->bitrateKbps));
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
            formats.begin(),
            formats.end(),
            [&](const Format& format) {
                return format.id == selector;
            });

        if (found == formats.end()) {
            return {
                nullptr,
                "Format code " + selector +
                " was not returned by YouTube."
            };
        }

        return {&*found, {}};
    }

    return resolveExtension(
        selector,
        formats,
        alreadySelectedIds);
}

} // namespace ytdl
