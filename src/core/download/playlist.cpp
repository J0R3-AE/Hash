#include "ytdl/downloader.hpp"

#include "ytdl/youtube_client.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace ytdl {

int downloadPlaylist(const std::string& playlistUrl,
                    const std::vector<std::string>& wantedSelectors,
                    const fs::path& outputDirectory,
                    std::string& error)
{
    PlaylistFetchResult playlist;
    if (!fetchPlaylist(playlistUrl, playlist)) {
        error = playlist.error.empty() ? "Failed to fetch playlist." : playlist.error;
        return -1;
    }

    if (playlist.videoIds.empty()) {
        error = "The playlist feed did not include any public video entries.";
        return 0;
    }

    int totalFailures = 0;
    int totalSuccesses = 0;
    for (std::size_t i = 0; i < playlist.videoIds.size(); ++i) {
        const std::string videoUrl = "https://www.youtube.com/watch?v=" + playlist.videoIds[i];
        VideoFetchResult video;
        if (!fetchVideo(videoUrl, video)) {
            ++totalFailures;
            std::cerr << "Skipping video " << (i + 1) << "/" << playlist.videoIds.size()
                      << " because it could not be fetched: " << video.error << '\n';
            continue;
        }

        const int result = downloadVideo(video.title, video.formats, wantedSelectors, outputDirectory);
        if (result == 0) {
            ++totalSuccesses;
        } else {
            totalFailures += result;
        }
    }

    std::cout << "\nPlaylist summary: " << totalSuccesses << " downloaded, "
              << (playlist.videoIds.size() - totalSuccesses - totalFailures) << " skipped, "
              << totalFailures << " failed.\n";

    return totalSuccesses > 0 ? 0 : 1;
}

} // namespace ytdl
