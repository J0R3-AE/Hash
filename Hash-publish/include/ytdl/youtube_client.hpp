#pragma once

#include "ytdl/types.hpp"

#include <string>
#include <vector>

namespace ytdl {

struct VideoFetchResult {
    std::string title;
    std::vector<Format> formats;
    bool ok = false;
    std::string error;
};

struct PlaylistFetchResult {
    std::vector<std::string> videoIds;
    bool ok = false;
    std::string error;
};

// Fetch a video's title and formats using the available YouTube player clients.
bool fetchVideo(const std::string& url, VideoFetchResult& result);

// Backward-compatible wrapper for callers that still want the older shape.
bool fetchVideoInfo(const std::string& url,
                    std::string& title,
                    std::vector<Format>& formats);

// Fetch the video IDs in a public playlist feed.
bool fetchPlaylist(const std::string& url, PlaylistFetchResult& result);

// Backward-compatible wrapper for callers that still want the older shape.
bool fetchPlaylistVideoIds(const std::string& url,
                          std::vector<std::string>& videoIds,
                          std::string& error);

} // namespace ytdl
