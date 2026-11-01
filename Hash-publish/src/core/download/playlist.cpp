
#include "ytdl/downloader.hpp"

#include "ytdl/youtube_client.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace ytdl {
namespace {

constexpr auto kDelayBetweenVideos =
    std::chrono::milliseconds(750);

constexpr auto kBotChallengeCooldown =
    std::chrono::seconds(5);

constexpr int kMaxBotChallenges = 2;

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

bool isBotChallenge(const std::string& error)
{
    const std::string message = lowercase(error);

    return message.find("not a bot") != std::string::npos
        || message.find("bot verification") != std::string::npos
        || (message.find("sign in") != std::string::npos
            && message.find("bot") != std::string::npos);
}

} // namespace

int downloadPlaylist(
    const std::string& playlistUrl,
    const std::vector<std::string>& wantedSelectors,
    const fs::path& outputDirectory,
    std::string& error)
{
    error.clear();

    PlaylistFetchResult playlist;

    if (!fetchPlaylist(playlistUrl, playlist)) {
        error = playlist.error.empty()
            ? "Failed to fetch playlist."
            : playlist.error;

        return -1;
    }

    if (playlist.videoIds.empty()) {
        error =
            "The playlist feed did not include any public video entries.";

        return -1;
    }

    const std::size_t totalVideos = playlist.videoIds.size();

    int fullyDownloaded = 0;
    int partiallyFailed = 0;
    int skipped = 0;
    int formatFailures = 0;
    int consecutiveBotChallenges = 0;

    std::size_t processed = 0;
    bool stoppedByBotChallenge = false;

    std::cout
        << "Starting playlist download: "
        << totalVideos << " videos.\n";

    for (std::size_t i = 0; i < totalVideos; ++i) {
        // Avoid sending a burst of metadata requests to YouTube.
        if (i > 0) {
            std::this_thread::sleep_for(kDelayBetweenVideos);
        }

        const std::string videoUrl =
            "https://www.youtube.com/watch?v="
            + playlist.videoIds[i];

        std::cout
            << "\n[" << (i + 1) << "/" << totalVideos
            << "] Fetching video information...\n";

        VideoFetchResult video;

        if (!fetchVideo(videoUrl, video)) {
            ++skipped;
            processed = i + 1;

            std::cerr
                << "Skipping video "
                << (i + 1) << "/" << totalVideos
                << " because it could not be fetched: "
                << video.error << '\n';

            if (isBotChallenge(video.error)) {
                ++consecutiveBotChallenges;

                if (consecutiveBotChallenges >= kMaxBotChallenges) {
                    stoppedByBotChallenge = true;

                    std::cerr
                        << "\nStopping playlist requests after "
                        << consecutiveBotChallenges
                        << " bot-verification challenges.\n"
                        << "Use a valid signed-in cookies.txt file "
                           "or try again later.\n";

                    break;
                }

                std::cout
                    << "YouTube requested bot verification. "
                       "Pausing before the next attempt...\n";

                std::this_thread::sleep_for(kBotChallengeCooldown);
            }

            continue;
        }

        // Reset the streak after a successful metadata fetch.
        consecutiveBotChallenges = 0;

        const int failures = downloadVideo(
            video.title,
            video.formats,
            wantedSelectors,
            outputDirectory);

        processed = i + 1;

        if (failures == 0) {
            ++fullyDownloaded;
        }
        else {
            ++partiallyFailed;
            formatFailures += failures;

            std::cerr
                << "Video "
                << (i + 1) << "/" << totalVideos
                << " had " << failures
                << " failed format selection(s).\n";
        }
    }

    std::cout << "\nPlaylist summary:\n"
              << "  Fully downloaded: " << fullyDownloaded << '\n'
              << "  Partially failed: " << partiallyFailed << '\n'
              << "  Skipped:          " << skipped << '\n'
              << "  Format failures:  " << formatFailures << '\n';

    const std::size_t notAttempted =
        totalVideos - processed;

    if (notAttempted > 0) {
        std::cout
            << "  Not attempted:     "
            << notAttempted << '\n';
    }

    if (stoppedByBotChallenge) {
        error =
            "Playlist processing stopped after repeated "
            "YouTube bot-verification challenges.";

        return -1;
    }

    if (fullyDownloaded == 0 &&
        partiallyFailed == 0) {
        error =
            "No videos were downloaded successfully. "
            "Review the skipped-video errors above.";

        return 1;
    }

    return partiallyFailed == 0 && skipped == 0 ? 0 : 1;
}

} // namespace ytdl
