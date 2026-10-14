#pragma once

#include "ytdl/types.hpp"

#include <string>
#include <vector>

namespace ytdl {

std::string trim(const std::string& value);
std::string sanitizeFilename(std::string name);
std::string extractVideoId(const std::string& input);
std::string extractPlaylistId(const std::string& input);
bool isPlaylistUrl(const std::string& input);
std::vector<std::string> parseFormatCodes(const std::string& input);
std::vector<std::string> parseDownloadSelectors(const std::string& input);
std::string normalizeBrowserName(const std::string& input);
std::string readConfigValue(const std::string& configPath,
                           const std::string& key);
std::string noCookiesWarningMessage();
std::string browserUserAgentForName(const std::string& browser);
bool isVideoRestrictedOrUnavailable(const std::string& status,
                                     const std::string& reason);
std::vector<std::string> summarizeFormatEntries(
    const std::vector<Format>& formats);
std::vector<std::string> summarizeFormatResolutions(
    const std::vector<Format>& formats);

} // namespace ytdl
