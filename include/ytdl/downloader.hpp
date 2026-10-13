#pragma once

#include "ytdl/types.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ytdl {

void listFormats(const std::vector<Format>& formats);

int downloadVideo(const std::string& title,
                  const std::vector<Format>& formats,
                  const std::vector<std::string>& wantedSelectors,
                  const std::filesystem::path& outputDirectory);

int downloadPlaylist(const std::string& playlistUrl,
                    const std::vector<std::string>& wantedSelectors,
                    const std::filesystem::path& outputDirectory,
                    std::string& error);

// Backward-compatible wrapper retained for older call sites.
int downloadFormats(const std::string& title,
                    const std::vector<Format>& formats,
                    const std::vector<std::string>& wantedSelectors,
                    const std::filesystem::path& outputDirectory);

} // namespace ytdl
