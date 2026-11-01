#pragma once

#include <string>

namespace ytdl {

// A YouTube Innertube client profile used when requesting playback metadata.
struct ClientProfile {
    std::string name;
    std::string version;
    std::string numericId;
    std::string apiKey;
    std::string userAgent;
    std::string extraClientFields;
};

// Metadata for a single format returned by YouTube.
struct Format {
    std::string id;
    std::string ext;
    std::string resolution;
    std::string bitrateKbps;
    std::string fps;
    std::string videoCodec;
    std::string audioCodec;
    std::string note;
    std::string url;
};

} // namespace ytdl
