#pragma once

#include "ytdl/types.hpp"

#include <filesystem>
#include <string>

namespace ytdl {

// Configure optional Netscape-format cookies for subsequent HTTP requests.
void setCookieFile(const std::string& path);

// Select a browser profile used to synthesize the default desktop user agent.
void setBrowserProfile(const std::string& browser);
std::string getBrowserProfileName();

// Convert a Firefox/Chromium SQLite cookie database into a Netscape cookies.txt file.
bool convertSQLiteCookiesToNetscape(const std::string& sqliteDbPath,
                                    const std::string& outputPath,
                                    std::string& error);

bool httpPostJson(const std::string& endpoint,
                  const std::string& body,
                  const ClientProfile& client,
                  std::string& responseBody,
                  std::string& error);

bool httpGetText(const std::string& url,
                 std::string& responseBody,
                 std::string& error);

bool downloadUrl(const std::string& url,
                 const std::filesystem::path& destination,
                 std::string& error);

} // namespace ytdl
