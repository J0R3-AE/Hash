#include "ytdl/http_client.hpp"

#include "ytdl/utils.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

namespace ytdl {
namespace {

std::string gCookieFile;
std::string gBrowserProfile = "auto";

} // namespace

void setCookieFile(const std::string& path)
{
    gCookieFile = path;
}

std::string getCookieFile()
{
    return gCookieFile;
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

        const auto has = [&](const std::string& col) {
            return std::find(columns.begin(), columns.end(), col) != columns.end();
        };

        if (table == "moz_cookies" || table == "cookies" || table == "moz_cookies_v2") {
            selectedTable = table;
            hostCol = has("host") ? "host" : (has("host_key") ? "host_key" : "host");
            pathCol = "path";
            secureCol = has("issecure") ? "isSecure" : (has("is_secure") ? "is_secure" : "isSecure");
            expiryCol = has("expiry") ? "expiry" : (has("expires_utc") ? "expires_utc" : "expiry");
            nameCol = "name";
            valueCol = "value";
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

        if (host.empty() || name.empty())
            continue;

        long long expiry = expiryRaw;
        if (expiry > 4102444800LL)
            expiry /= 1000000LL;

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

} // namespace ytdl
