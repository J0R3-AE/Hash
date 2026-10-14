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

    const std::string cookieFile = getCookieFile();

    curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
    if (!cookieFile.empty())
        curl_easy_setopt(curl, CURLOPT_COOKIEFILE, cookieFile.c_str());
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

    const std::string browserProfile = getBrowserProfileName();

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     browserUserAgentForName(browserProfile).c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
    const std::string cookieFile = getCookieFile();
    if (!cookieFile.empty())
        curl_easy_setopt(curl, CURLOPT_COOKIEFILE, cookieFile.c_str());

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

    const std::string browserProfile = getBrowserProfileName();
    const std::string cookieFile = getCookieFile();

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    if (!cookieFile.empty())
        curl_easy_setopt(curl, CURLOPT_COOKIEFILE, cookieFile.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     browserUserAgentForName(browserProfile).c_str());
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
