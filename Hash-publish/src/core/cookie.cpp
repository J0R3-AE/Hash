#include "ytdl/http_client.hpp"

#include "ytdl/utils.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

namespace ytdl {
namespace {

std::string gCookieFile;
std::string gBrowserProfile = "auto";

std::string lowercase(std::string value)
{
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    return value;
}

std::string quoteIdentifier(const std::string& identifier)
{
    std::string quoted = "\"";
    for (char ch : identifier) {
        if (ch == '"') {
            quoted += "\"\"";
        } else {
            quoted += ch;
        }
    }
    quoted += '"';
    return quoted;
}

struct Column {
    std::string name;
    std::string normalized;
};

struct Table {
    std::string name;
    std::string normalized;
    std::vector<Column> columns;

    bool has(const std::string& column) const
    {
        return std::any_of(
            columns.begin(), columns.end(),
            [&](const Column& entry) {
                return entry.normalized == column;
            });
    }

    std::string actual(const std::string& column) const
    {
        for (const Column& entry : columns) {
            if (entry.normalized == column) {
                return entry.name;
            }
        }
        return {};
    }
};

bool readTableSchema(sqlite3* db, Table& table, std::string& error)
{
    sqlite3_stmt* statement = nullptr;
    const std::string sql =
        "PRAGMA table_info(" + quoteIdentifier(table.name) + ");";

    if (sqlite3_prepare_v2(
            db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        return false;
    }

    int status = SQLITE_OK;
    while ((status = sqlite3_step(statement)) == SQLITE_ROW) {
        const unsigned char* name = sqlite3_column_text(statement, 1);
        if (!name) {
            continue;
        }

        const std::string actualName(
            reinterpret_cast<const char*>(name));

        table.columns.push_back({
            actualName,
            lowercase(actualName)
        });
    }

    if (status != SQLITE_DONE) {
        error = sqlite3_errmsg(db);
        sqlite3_finalize(statement);
        return false;
    }

    sqlite3_finalize(statement);
    return true;
}

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

bool convertSQLiteCookiesToNetscape(
    const std::string& sqliteDbPath,
    const std::string& outputPath,
    std::string& error)
{
    error.clear();

    sqlite3* db = nullptr;
    const int openStatus = sqlite3_open_v2(
        sqliteDbPath.c_str(),
        &db,
        SQLITE_OPEN_READONLY,
        nullptr);

    if (openStatus != SQLITE_OK) {
        error = db ? sqlite3_errmsg(db)
                   : "SQLite could not allocate a database handle.";
        if (db) {
            sqlite3_close(db);
        }
        return false;
    }

    sqlite3_busy_timeout(db, 3000);

    // Discover cookie-like tables rather than assuming every browser
    // uses the same table name.
    sqlite3_stmt* tableStatement = nullptr;
    const char* tableSql =
        "SELECT name FROM sqlite_master "
        "WHERE type='table' ORDER BY name;";

    if (sqlite3_prepare_v2(
            db, tableSql, -1, &tableStatement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        sqlite3_close(db);
        return false;
    }

    std::vector<Table> tables;
    int status = SQLITE_OK;

    while ((status = sqlite3_step(tableStatement)) == SQLITE_ROW) {
        const unsigned char* name = sqlite3_column_text(tableStatement, 0);
        if (!name) {
            continue;
        }

        Table table;
        table.name = reinterpret_cast<const char*>(name);
        table.normalized = lowercase(table.name);

        if (table.normalized.find("cookie") != std::string::npos) {
            tables.push_back(std::move(table));
        }
    }

    sqlite3_finalize(tableStatement);

    if (status != SQLITE_DONE) {
        error = sqlite3_errmsg(db);
        sqlite3_close(db);
        return false;
    }

    if (tables.empty()) {
        error = "No supported cookie table found in SQLite database.";
        sqlite3_close(db);
        return false;
    }

    Table* selected = nullptr;

    // Prefer browser-recognized tables; fall back to any cookie table
    // with the minimum columns required for Netscape cookie export.
    for (Table& table : tables) {
        std::string schemaError;
        if (!readTableSchema(db, table, schemaError)) {
            continue;
        }

        const bool hasHost = table.has("host") || table.has("host_key");
        if (hasHost && table.has("path") &&
            table.has("name") && table.has("value")) {
            if (table.normalized == "cookies" ||
                table.normalized == "moz_cookies" ||
                table.normalized == "moz_cookies_v2") {
                selected = &table;
                break;
            }
            if (!selected) {
                selected = &table;
            }
        }
    }

    if (!selected) {
        error = "No supported cookie columns found in SQLite database.";
        sqlite3_close(db);
        return false;
    }

    const std::string hostColumn = selected->has("host")
        ? selected->actual("host")
        : selected->actual("host_key");
    const std::string pathColumn = selected->actual("path");
    const std::string nameColumn = selected->actual("name");
    const std::string valueColumn = selected->actual("value");

    std::string secureExpression = "0";
    if (selected->has("is_secure")) {
        secureExpression = quoteIdentifier(selected->actual("is_secure"));
    } else if (selected->has("issecure")) {
        secureExpression = quoteIdentifier(selected->actual("issecure"));
    } else if (selected->has("secure")) {
        secureExpression = quoteIdentifier(selected->actual("secure"));
    }

    std::string expiryExpression = "0";
    const bool chromiumExpiry = selected->has("expires_utc");
    if (chromiumExpiry) {
        expiryExpression = quoteIdentifier(selected->actual("expires_utc"));
    } else if (selected->has("expiry")) {
        expiryExpression = quoteIdentifier(selected->actual("expiry"));
    } else if (selected->has("expires")) {
        expiryExpression = quoteIdentifier(selected->actual("expires"));
    }

    const bool hasEncryptedValue = selected->has("encrypted_value");
    const std::string encryptedExpression = hasEncryptedValue
        ? quoteIdentifier(selected->actual("encrypted_value"))
        : "NULL";

    const std::string query =
        "SELECT " + quoteIdentifier(hostColumn) + ", " +
        quoteIdentifier(pathColumn) + ", " +
        secureExpression + ", " +
        expiryExpression + ", " +
        quoteIdentifier(nameColumn) + ", " +
        quoteIdentifier(valueColumn) + ", " +
        encryptedExpression +
        " FROM " + quoteIdentifier(selected->name) +
        " WHERE " + quoteIdentifier(nameColumn) + " IS NOT NULL "
        "ORDER BY " + quoteIdentifier(hostColumn) + ", " +
        quoteIdentifier(pathColumn) + ", " +
        quoteIdentifier(nameColumn) + ";";

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(
            db, query.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        sqlite3_close(db);
        return false;
    }

    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "Could not open output file for Netscape cookie export: " + outputPath;
        sqlite3_finalize(statement);
        sqlite3_close(db);
        return false;
    }

    output << "# Netscape HTTP Cookie File\n";

    bool detectedEncryptedCookies = false;

    while ((status = sqlite3_step(statement)) == SQLITE_ROW) {
        const unsigned char* hostPtr = sqlite3_column_text(statement, 0);
        const unsigned char* pathPtr = sqlite3_column_text(statement, 1);
        const unsigned char* namePtr = sqlite3_column_text(statement, 4);
        const unsigned char* valuePtr = sqlite3_column_text(statement, 5);

        const int encryptedBytes = sqlite3_column_bytes(statement, 6);
        const void* encryptedData = sqlite3_column_blob(statement, 6);

        const std::string value = valuePtr
            ? std::string(reinterpret_cast<const char*>(valuePtr))
            : std::string();

        // Chrome/Edge commonly store the meaningful value in
        // encrypted_value and leave value empty. This converter does
        // not decrypt browser-protected values, so fail explicitly
        // instead of producing a misleading, incomplete cookie file.
        if (hasEncryptedValue && encryptedData && encryptedBytes > 0 &&
            value.empty()) {
            detectedEncryptedCookies = true;
            break;
        }

        if (!hostPtr || !namePtr || value.empty()) {
            continue;
        }

        const std::string host(
            reinterpret_cast<const char*>(hostPtr));
        const std::string path = pathPtr
            ? reinterpret_cast<const char*>(pathPtr)
            : "/";
        const std::string name(
            reinterpret_cast<const char*>(namePtr));

        if (host.empty() || name.empty()) {
            continue;
        }

        const int secure = sqlite3_column_int(statement, 2);
        const long long expiryRaw = sqlite3_column_int64(statement, 3);

        long long expiry = expiryRaw;

        if (chromiumExpiry && expiryRaw > 0) {
            // Chromium's expires_utc is microseconds since
            // 1601-01-01; Netscape cookies use Unix epoch seconds.
            constexpr long long kWindowsToUnixEpochSeconds =
                11644473600LL;
            expiry = expiryRaw / 1000000LL -
                     kWindowsToUnixEpochSeconds;
        }

        // Netscape's TRUE flag indicates a domain cookie. Chromium's
        // host_key and Firefox's host generally start with a dot for
        // domain cookies; host-only cookies should use FALSE.
        const bool domainCookie = !host.empty() && host.front() == '.';

        output << host << '\t'
               << (domainCookie ? "TRUE" : "FALSE") << '\t'
               << (path.empty() ? "/" : path) << '\t'
               << (secure ? "TRUE" : "FALSE") << '\t'
               << expiry << '\t'
               << name << '\t'
               << value << '\n';

        if (!output) {
            error = "Failed while writing Netscape cookies file: " + outputPath;
            sqlite3_finalize(statement);
            sqlite3_close(db);
            output.close();
            std::error_code ignored;
            fs::remove(outputPath, ignored);
            return false;
        }
    }

    if (detectedEncryptedCookies) {
        error =
            "This Chromium-based cookie database contains encrypted cookie values "
            "that this converter cannot decrypt. Export a Netscape-format "
            "cookies.txt from your signed-in browser session instead.";

        sqlite3_finalize(statement);
        sqlite3_close(db);
        output.close();
        std::error_code ignored;
        fs::remove(outputPath, ignored);
        return false;
    }

    if (status != SQLITE_DONE) {
        error = sqlite3_errmsg(db);
        sqlite3_finalize(statement);
        sqlite3_close(db);
        output.close();
        std::error_code ignored;
        fs::remove(outputPath, ignored);
        return false;
    }

    sqlite3_finalize(statement);
    sqlite3_close(db);
    output.close();

    if (!output) {
        error = "Failed to finish writing Netscape cookie file: " + outputPath;
        std::error_code ignored;
        fs::remove(outputPath, ignored);
        return false;
    }

#ifndef _WIN32
    if (::chmod(outputPath.c_str(), S_IRUSR | S_IWUSR) != 0) {
        error = "Could not restrict permissions on exported cookie file.";
        std::error_code ignored;
        fs::remove(outputPath, ignored);
        return false;
    }
#endif

    return true;
}

} // namespace ytdl
