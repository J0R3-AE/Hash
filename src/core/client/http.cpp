#include "ytdl/http_client.hpp"

#include "ytdl/utils.hpp"

#include <curl/curl.h>

#include <sqlite3.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace ytdl {
namespace {

std::string gCookieFile;
std::string gBrowserProfile = "auto";

} // namespace

void setCookieFile(const std::string& path)
{
    gCookieFile = path;
}

void setBrowserProfile(const std::string& browser)
{
    gBrowserProfile = normalizeBrowserName(browser);
}

std::string getBrowserProfileName()
{
    return gBrowserProfile;
}

bool convertSQLiteCookiesToNetscape(const std::string& sqliteDbPath,
                                    const std::string& outputPath,
                                    std::string& error)
{
    sqlite3* db = nullptr;
    const std::string uri = "file:" + sqliteDbPath + "?mode=ro&immutable=1";
    if (sqlite3_open_v2(uri.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(db ? db : nullptr);
        if (db) sqlite3_close(db);
        return false;
    }

    std::vector<std::string> tables;
    sqlite3_stmt* tableStmt = nullptr;
    const int tableStatus = sqlite3_prepare_v2(
        db,
        "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name;",
        -1,
        &tableStmt,
        nullptr);
    if (tableStatus == SQLITE_OK) {
        while (sqlite3_step(tableStmt) == SQLITE_ROW) {
            const unsigned char* name = sqlite3_column_text(tableStmt, 0);
            if (name) {
                std::string tableName(reinterpret_cast<const char*>(name));
                std::transform(tableName.begin(), tableName.end(), tableName.begin(), [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                });
                if (tableName.find("cookie") != std::string::npos) {
                    tables.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(tableStmt, 0)));
                }
            }
        }
        sqlite3_finalize(tableStmt);
    }

    if (tableStatus != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        sqlite3_finalize(tableStmt);
        sqlite3_close(db);
        return false;
    }

    if (tables.empty()) {
        error = "No supported cookie table found in sqlite database.";
        sqlite3_close(db);
        return false;
    }

    std::string selectedTable;
    std::string hostCol, pathCol, secureCol, expiryCol, nameCol, valueCol;
    for (const std::string& table : tables) {
        sqlite3_stmt* infoStmt = nullptr;
        const std::string infoQuery = "PRAGMA table_info('" + table + "');";
        if (sqlite3_prepare_v2(db, infoQuery.c_str(), -1, &infoStmt, nullptr) != SQLITE_OK) {
            continue;
        }

        std::vector<std::string> columns;
        while (sqlite3_step(infoStmt) == SQLITE_ROW) {
            const unsigned char* colName = sqlite3_column_text(infoStmt, 1);
            if (colName) {
                std::string value(reinterpret_cast<const char*>(colName));
                std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                });
                columns.push_back(value);
            }
        }
        sqlite3_finalize(infoStmt);

        auto has = [&](const std::string& col) {
            return std::find(columns.begin(), columns.end(), col) != columns.end();
        };

        if (table == "moz_cookies" || table == "cookies" || table == "moz_cookies_v2") {
            selectedTable = table;
            hostCol = has("host") ? "host" : (has("host_key") ? "host_key" : "host");
            pathCol = has("path") ? "path" : "path";
            secureCol = has("issecure") ? "isSecure" : (has("is_secure") ? "is_secure" : "isSecure");
            expiryCol = has("expiry") ? "expiry" : (has("expires_utc") ? "expires_utc" : "expiry");
            nameCol = has("name") ? "name" : "name";
            valueCol = has("value") ? "value" : "value";
            break;
        }

        if (has("host") && has("path") && has("name") && has("value")) {
            selectedTable = table;
            hostCol = "host";
            pathCol = "path";
            secureCol = has("issecure") ? "isSecure" : (has("is_secure") ? "is_secure" : "isSecure");
            expiryCol = has("expiry") ? "expiry" : (has("expires_utc") ? "expires_utc" : "expiry");
            nameCol = "name";
            valueCol = "value";
            break;
        }
    }

    if (selectedTable.empty()) {
        error = "No supported cookie columns found in sqlite database.";
        sqlite3_close(db);
        return false;
    }

    const std::string query =
        "SELECT " + hostCol + ", " + pathCol + ", " + secureCol + ", " + expiryCol + ", " + nameCol + ", " + valueCol +
        " FROM '" + selectedTable + "' WHERE " + nameCol + " IS NOT NULL ORDER BY " + hostCol + ", " + pathCol + ", " + nameCol + ";";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        sqlite3_close(db);
        return false;
    }

    std::ofstream out(outputPath, std::ios::binary);
    if (!out) {
        error = "Could not open output file for Netscape cookie export: " + outputPath;
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return false;
    }
    out << "# Netscape HTTP Cookie File\n";

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* hostPtr = sqlite3_column_text(stmt, 0);
        const unsigned char* pathPtr = sqlite3_column_text(stmt, 1);
        const unsigned char* namePtr = sqlite3_column_text(stmt, 4);
        const unsigned char* valuePtr = sqlite3_column_text(stmt, 5);
        if (!hostPtr || !namePtr || !valuePtr) {
            continue;
        }

        const std::string host(reinterpret_cast<const char*>(hostPtr));
        const std::string path = pathPtr ? reinterpret_cast<const char*>(pathPtr) : "/";
        const std::string name(reinterpret_cast<const char*>(namePtr));
        const std::string value(reinterpret_cast<const char*>(valuePtr));
        const int secure = sqlite3_column_int(stmt, 2);
        const long long expiryRaw = sqlite3_column_int64(stmt, 3);

        if (host.empty() || name.empty()) {
            continue;
        }

        long long expiry = expiryRaw;
        if (expiry > 4102444800LL) {
            expiry /= 1000000LL;
        }

        const std::string domainFlag = (host.find('.') != std::string::npos && host != "localhost") ? "TRUE" : "FALSE";
        out << host << "\t"
            << domainFlag << "\t"
            << (path.empty() ? "/" : path) << "\t"
            << (secure ? "TRUE" : "FALSE") << "\t"
            << expiry << "\t"
            << name << "\t"
            << value << "\n";
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    out.close();
    return true;
}

static std::size_t appendToString(char* data, std::size_t size, std::size_t count,
                           void* userData)
{
    const std::size_t bytes = size * count;
    auto* output = static_cast<std::string*>(userData);
    output->append(data, bytes);
    return bytes;
}

static std::size_t writeToFile(char* data, std::size_t size, std::size_t count,
                        void* userData)
{
    auto* file = static_cast<std::FILE*>(userData);
    return std::fwrite(data, size, count, file) * size;
}

bool httpPostJson(const std::string& endpoint, const std::string& body,
                  const ClientProfile& client,
                  std::string& responseBody, std::string& error)
{
    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "Could not initialize libcurl.";
        return false;
    }

    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    const std::string clientNameHeader =
        std::string("X-YouTube-Client-Name: ") + client.numericId;
    const std::string clientVersionHeader =
        std::string("X-YouTube-Client-Version: ") + client.version;
    headers = curl_slist_append(headers, clientNameHeader.c_str());
    headers = curl_slist_append(headers, clientVersionHeader.c_str());
    headers = curl_slist_append(headers, "Origin: https://www.youtube.com");
    headers = curl_slist_append(headers, "Referer: https://www.youtube.com/");

    curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
    if (!gCookieFile.empty())
        curl_easy_setopt(curl, CURLOPT_COOKIEFILE, gCookieFile.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, client.userAgent);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    const CURLcode result = curl_easy_perform(curl);
    long httpStatus = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);

    bool success = result == CURLE_OK && httpStatus >= 200 && httpStatus < 300;
    if (result != CURLE_OK)
        error = curl_easy_strerror(result);
    else if (!success)
        error = "YouTube returned HTTP " + std::to_string(httpStatus) + ".";

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return success;
}

bool httpGetText(const std::string& url, std::string& responseBody,
                 std::string& error)
{
    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "Could not initialize libcurl.";
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     browserUserAgentForName(gBrowserProfile).c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
    if (!gCookieFile.empty())
        curl_easy_setopt(curl, CURLOPT_COOKIEFILE, gCookieFile.c_str());

    const CURLcode result = curl_easy_perform(curl);
    long httpStatus = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
    const bool success = result == CURLE_OK && httpStatus >= 200 && httpStatus < 300;
    if (result != CURLE_OK)
        error = curl_easy_strerror(result);
    else if (!success)
        error = "YouTube homepage returned HTTP " + std::to_string(httpStatus) + ".";

    curl_easy_cleanup(curl);
    return success;
}

bool downloadUrl(const std::string& url, const fs::path& destination,
                 std::string& error)
{
    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "Could not initialize libcurl.";
        return false;
    }

    std::FILE* file = std::fopen(destination.string().c_str(), "wb");
    if (!file) {
        error = "Could not create output file: " + destination.string();
        curl_easy_cleanup(curl);
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    if (!gCookieFile.empty())
        curl_easy_setopt(curl, CURLOPT_COOKIEFILE, gCookieFile.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     browserUserAgentForName(gBrowserProfile).c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);

    const CURLcode result = curl_easy_perform(curl);
    long httpStatus = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
    std::fclose(file);

    const bool success = result == CURLE_OK && httpStatus >= 200 && httpStatus < 300;
    if (!success) {
        if (result != CURLE_OK)
            error = curl_easy_strerror(result);
        else
            error = "HTTP " + std::to_string(httpStatus);

        std::error_code ignored;
        fs::remove(destination, ignored);
    }

    curl_easy_cleanup(curl);
    return success;
}

} // namespace ytdl
