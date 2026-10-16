
#include "ytdl/http_client.hpp"

#include "ytdl/utils.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace ytdl
{
    namespace
    {

        constexpr int MAX_RETRIES = 3;
        constexpr long CONNECT_TIMEOUT_SECONDS = 30;
        constexpr long TEXT_TIMEOUT_SECONDS = 90;
        constexpr long LOW_SPEED_LIMIT_BYTES = 1;
        constexpr long LOW_SPEED_TIMEOUT_SECONDS = 60;
        constexpr long DOWNLOAD_BUFFER_SIZE = 256L * 1024L;

        // ============================================================
        // Response callbacks
        // ============================================================

        static std::size_t appendToString(
            char *data,
            std::size_t size,
            std::size_t count,
            void *userData)
        {
            if (size != 0 &&
                count > std::numeric_limits<std::size_t>::max() / size)
            {
                return 0;
            }

            const std::size_t bytes = size * count;

            auto *output = static_cast<std::string *>(userData);

            if (!output)
            {
                return 0;
            }

            try
            {
                output->append(data, bytes);
                return bytes;
            }
            catch (...)
            {
                // Never allow C++ exceptions to escape through libcurl.
                return 0;
            }
        }

        struct DownloadContext
        {
            std::FILE *file = nullptr;

            curl_off_t resumeOffset = 0;
            long responseStatus = 0;

            bool rangeIgnored = false;
            bool writeFailed = false;
        };

        // Tracks HTTP response status lines so a server that ignores a
        // resume request cannot append a complete file to a partial file.
        static std::size_t inspectDownloadHeaders(
            char *data,
            std::size_t size,
            std::size_t count,
            void *userData)
        {
            if (size != 0 &&
                count > std::numeric_limits<std::size_t>::max() / size)
            {
                return 0;
            }

            const std::size_t bytes = size * count;

            auto *context =
                static_cast<DownloadContext *>(userData);

            if (!context)
            {
                return 0;
            }

            const std::string line(data, bytes);

            if (line.compare(0, 5, "HTTP/") == 0)
            {
                const std::size_t space = line.find(' ');

                if (space != std::string::npos)
                {
                    std::size_t position = space + 1;

                    while (position < line.size() &&
                           std::isspace(
                               static_cast<unsigned char>(line[position])))
                    {
                        ++position;
                    }

                    long status = 0;
                    bool foundDigit = false;

                    while (position < line.size() &&
                           std::isdigit(
                               static_cast<unsigned char>(line[position])))
                    {
                        foundDigit = true;

                        status = status * 10 + (line[position] - '0');

                        ++position;
                    }

                    if (foundDigit)
                    {
                        context->responseStatus = status;
                    }
                }
            }

            return bytes;
        }

        static std::size_t writeToFile(
            char *data,
            std::size_t size,
            std::size_t count,
            void *userData)
        {
            if (size != 0 &&
                count > std::numeric_limits<std::size_t>::max() / size)
            {
                return 0;
            }

            const std::size_t bytes = size * count;

            auto *context =
                static_cast<DownloadContext *>(userData);

            if (!context || !context->file)
            {
                return 0;
            }

            // If a server ignores Range and responds with HTTP 200,
            // do not append the entire response to the existing partial.
            if (context->resumeOffset > 0 &&
                context->responseStatus == 200)
            {
                context->rangeIgnored = true;
                return 0;
            }

            const std::size_t written =
                std::fwrite(data, 1, bytes, context->file);

            if (written != bytes)
            {
                context->writeFailed = true;
            }

            return written;
        }

        // ============================================================
        // Retry handling
        // ============================================================

        static bool isRetryableCurlError(CURLcode result)
        {
            switch (result)
            {
            case CURLE_COULDNT_RESOLVE_PROXY:
            case CURLE_COULDNT_RESOLVE_HOST:
            case CURLE_COULDNT_CONNECT:
            case CURLE_OPERATION_TIMEDOUT:
            case CURLE_SEND_ERROR:
            case CURLE_RECV_ERROR:
            case CURLE_GOT_NOTHING:
            case CURLE_PARTIAL_FILE:
            case CURLE_AGAIN:
                return true;

            default:
                return false;
            }
        }

        static bool isRetryableHttpStatus(long status)
        {
            switch (status)
            {
            case 408: // Request timeout
            case 425: // Too early
            case 429: // Too many requests
            case 500: // Internal server error
            case 502: // Bad gateway
            case 503: // Service unavailable
            case 504: // Gateway timeout
                return true;

            default:
                return false;
            }
        }

        static bool shouldRetry(CURLcode result, long status)
        {
            return isRetryableCurlError(result) || isRetryableHttpStatus(status);
        }

        static void waitBeforeRetry(int attempt)
        {
            // Exponential backoff: 250 ms, 500 ms, 1 second.
            const auto delay =
                std::chrono::milliseconds(250L * (1L << attempt));

            std::this_thread::sleep_for(delay);
        }

        static std::string curlErrorMessage(
            CURLcode result,
            const char *errorBuffer)
        {
            if (errorBuffer && errorBuffer[0] != '\0')
            {
                return errorBuffer;
            }

            return curl_easy_strerror(result);
        }

        // Retries transient failures for HTTP text/API requests.
        // The response body is cleared before every attempt.
        static bool performTextRequest(
            CURL *curl,
            std::string &responseBody,
            std::string &error)
        {
            char errorBuffer[CURL_ERROR_SIZE] = {};

            curl_easy_setopt(
                curl,
                CURLOPT_ERRORBUFFER,
                errorBuffer);

            for (int attempt = 0; attempt < MAX_RETRIES; ++attempt)
            {
                responseBody.clear();
                errorBuffer[0] = '\0';

                const CURLcode result =
                    curl_easy_perform(curl);

                long httpStatus = 0;

                curl_easy_getinfo(
                    curl,
                    CURLINFO_RESPONSE_CODE,
                    &httpStatus);

                if (result == CURLE_OK &&
                    httpStatus >= 200 &&
                    httpStatus < 300)
                {
                    error.clear();
                    return true;
                }

                if (attempt + 1 < MAX_RETRIES &&
                    shouldRetry(result, httpStatus))
                {
                    waitBeforeRetry(attempt);
                    continue;
                }

                if (result != CURLE_OK)
                {
                    error = curlErrorMessage(result, errorBuffer);

                    if (httpStatus > 0)
                    {
                        error += " (HTTP " + std::to_string(httpStatus) + ")";
                    }
                }
                else
                {
                    error = "HTTP " + std::to_string(httpStatus);
                }

                return false;
            }

            error = "HTTP request failed after retries.";
            return false;
        }

        // ============================================================
        // File handling
        // ============================================================

        // Use wide-character paths on Windows so output paths containing
        // non-ASCII characters work correctly with MinGW/Windows.
        static std::FILE *openOutputFile(
            const fs::path &path,
            bool append)
        {
#ifdef _WIN32
            return _wfopen(
                path.c_str(),
                append ? L"ab" : L"wb");
#else
            return std::fopen(
                path.c_str(),
                append ? "ab" : "wb");
#endif
        }

        static void removeFileNoThrow(const fs::path &path)
        {
            std::error_code ignored;
            fs::remove(path, ignored);
        }

        static bool prepareOutputDirectory(
            const fs::path &destination,
            std::string &error)
        {
            if (destination.empty() ||
                destination.filename().empty())
            {
                error = "The output filename is empty.";
                return false;
            }

            const fs::path parent = destination.parent_path();

            if (parent.empty())
            {
                return true;
            }

            std::error_code fsError;

            fs::create_directories(parent, fsError);

            if (fsError)
            {
                error = "Cannot create output directory '" + parent.u8string() + "': " + fsError.message();

                return false;
            }

            if (!fs::is_directory(parent, fsError) || fsError)
            {
                error = "Output parent is not a valid directory: " + parent.u8string();

                return false;
            }

            return true;
        }

        // Move a fully downloaded temporary file into its final location.
        static bool commitDownloadedFile(
            const fs::path &temporary,
            const fs::path &destination,
            std::string &error)
        {
            std::error_code renameError;

            fs::rename(temporary, destination, renameError);

#ifdef _WIN32
            // Windows filesystem rename generally cannot replace an
            // existing destination. Try removing it only after the new
            // download has completed successfully.
            if (renameError)
            {
                std::error_code existsError;

                const bool destinationExists =
                    fs::exists(destination, existsError);

                if (destinationExists && !existsError)
                {
                    std::error_code removeError;

                    fs::remove(destination, removeError);

                    if (!removeError)
                    {
                        renameError.clear();

                        fs::rename(
                            temporary,
                            destination,
                            renameError);
                    }
                }
            }
#endif

            if (renameError)
            {
                error = "Cannot move completed download to '" + destination.u8string() + "': " + renameError.message();

                return false;
            }

            return true;
        }

        // ============================================================
        // Common curl configuration
        // ============================================================

        static void configureTextRequest(
            CURL *curl,
            const std::string &url)
        {
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

            curl_easy_setopt(
                curl,
                CURLOPT_FOLLOWLOCATION,
                1L);

            curl_easy_setopt(
                curl,
                CURLOPT_MAXREDIRS,
                10L);

            curl_easy_setopt(
                curl,
                CURLOPT_CONNECTTIMEOUT,
                CONNECT_TIMEOUT_SECONDS);

            curl_easy_setopt(
                curl,
                CURLOPT_TIMEOUT,
                TEXT_TIMEOUT_SECONDS);

            curl_easy_setopt(
                curl,
                CURLOPT_NOSIGNAL,
                1L);

            curl_easy_setopt(
                curl,
                CURLOPT_ACCEPT_ENCODING,
                "");

            curl_easy_setopt(
                curl,
                CURLOPT_HTTP_VERSION,
                CURL_HTTP_VERSION_1_1);
        }

    } // namespace

    // ============================================================
    // HTTP POST: YouTube API JSON
    // ============================================================

    bool httpPostJson(
        const std::string &endpoint,
        const std::string &body,
        const ClientProfile &client,
        std::string &responseBody,
        std::string &error)
    {
        responseBody.clear();
        error.clear();

        CURL *curl = curl_easy_init();

        if (!curl)
        {
            error = "Could not initialize libcurl.";
            return false;
        }

        curl_slist *headers = nullptr;

        const std::string clientNameHeader =
            "X-YouTube-Client-Name: " + client.numericId;

        const std::string clientVersionHeader =
            "X-YouTube-Client-Version: " + client.version;

        const std::vector<std::string> headerValues = {
            "Content-Type: application/json",
            clientNameHeader,
            clientVersionHeader,
            "Origin: https://www.youtube.com",
            "Referer: https://www.youtube.com/"};

        bool headersOk = true;

        for (const auto &value : headerValues)
        {
            curl_slist *updated =
                curl_slist_append(headers, value.c_str());

            if (!updated)
            {
                headersOk = false;
                break;
            }

            headers = updated;
        }

        if (!headersOk)
        {
            if (headers)
            {
                curl_slist_free_all(headers);
            }

            curl_easy_cleanup(curl);

            error = "Could not allocate HTTP request headers.";
            return false;
        }

        const std::string cookieFile = getCookieFile();

        configureTextRequest(curl, endpoint);

        curl_easy_setopt(
            curl,
            CURLOPT_HTTPHEADER,
            headers);

        curl_easy_setopt(
            curl,
            CURLOPT_USERAGENT,
            client.userAgent.c_str());

        curl_easy_setopt(
            curl,
            CURLOPT_POST,
            1L);

        curl_easy_setopt(
            curl,
            CURLOPT_POSTFIELDS,
            body.data());

        curl_easy_setopt(
            curl,
            CURLOPT_POSTFIELDSIZE_LARGE,
            static_cast<curl_off_t>(body.size()));

        if (!cookieFile.empty())
        {
            curl_easy_setopt(
                curl,
                CURLOPT_COOKIEFILE,
                cookieFile.c_str());
        }

        curl_easy_setopt(
            curl,
            CURLOPT_WRITEFUNCTION,
            appendToString);

        curl_easy_setopt(
            curl,
            CURLOPT_WRITEDATA,
            &responseBody);

        const bool success =
            performTextRequest(curl, responseBody, error);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        return success;
    }

    // ============================================================
    // HTTP GET: text / YouTube page requests
    // ============================================================

    bool httpGetText(
        const std::string &url,
        std::string &responseBody,
        std::string &error)
    {
        responseBody.clear();
        error.clear();

        CURL *curl = curl_easy_init();

        if (!curl)
        {
            error = "Could not initialize libcurl.";
            return false;
        }

        const std::string browserProfile =
            getBrowserProfileName();

        const std::string userAgent =
            browserUserAgentForName(browserProfile);

        const std::string cookieFile = getCookieFile();

        configureTextRequest(curl, url);

        curl_easy_setopt(
            curl,
            CURLOPT_USERAGENT,
            userAgent.c_str());

        if (!cookieFile.empty())
        {
            curl_easy_setopt(
                curl,
                CURLOPT_COOKIEFILE,
                cookieFile.c_str());
        }

        curl_easy_setopt(
            curl,
            CURLOPT_WRITEFUNCTION,
            appendToString);

        curl_easy_setopt(
            curl,
            CURLOPT_WRITEDATA,
            &responseBody);

        const bool success =
            performTextRequest(curl, responseBody, error);

        curl_easy_cleanup(curl);

        return success;
    }

    // ============================================================
    // HTTP GET: media download
    // ============================================================

    bool downloadUrl(
        const std::string &url,
        const fs::path &destination,
        std::string &error)
    {
        error.clear();

        if (url.empty())
        {
            error = "The download URL is empty.";
            return false;
        }

        if (!prepareOutputDirectory(destination, error))
        {
            return false;
        }

        CURL *curl = curl_easy_init();

        if (!curl)
        {
            error = "Could not initialize libcurl.";
            return false;
        }

        const std::string browserProfile =
            getBrowserProfileName();

        const std::string userAgent =
            browserUserAgentForName(browserProfile);

        const std::string cookieFile = getCookieFile();

        // Download to a temporary sibling file. The final output is
        // only replaced when the transfer has completed successfully.
        fs::path temporary = destination;
        temporary += ".part";

        std::error_code fsError;

        fs::remove(temporary, fsError);

        if (fsError)
        {
            error = "Cannot remove stale partial file '" + temporary.u8string() + "': " + fsError.message();

            curl_easy_cleanup(curl);
            return false;
        }

        curl_easy_setopt(
            curl,
            CURLOPT_URL,
            url.c_str());

        curl_easy_setopt(
            curl,
            CURLOPT_USERAGENT,
            userAgent.c_str());

        if (!cookieFile.empty())
        {
            curl_easy_setopt(
                curl,
                CURLOPT_COOKIEFILE,
                cookieFile.c_str());
        }

        curl_easy_setopt(
            curl,
            CURLOPT_FOLLOWLOCATION,
            1L);

        curl_easy_setopt(
            curl,
            CURLOPT_MAXREDIRS,
            10L);

        curl_easy_setopt(
            curl,
            CURLOPT_CONNECTTIMEOUT,
            CONNECT_TIMEOUT_SECONDS);

        // Do not impose a total download timeout. Large media files
        // may legitimately take a long time on slower connections.
        curl_easy_setopt(
            curl,
            CURLOPT_NOSIGNAL,
            1L);

        curl_easy_setopt(
            curl,
            CURLOPT_LOW_SPEED_LIMIT,
            LOW_SPEED_LIMIT_BYTES);

        curl_easy_setopt(
            curl,
            CURLOPT_LOW_SPEED_TIME,
            LOW_SPEED_TIMEOUT_SECONDS);

        // Larger receive buffers may reduce callback overhead.
        // This is a request to libcurl, not a guaranteed transfer speedup.
        curl_easy_setopt(
            curl,
            CURLOPT_BUFFERSIZE,
            DOWNLOAD_BUFFER_SIZE);

        curl_easy_setopt(
            curl,
            CURLOPT_HTTP_VERSION,
            CURL_HTTP_VERSION_1_1);

        // Do not ask for automatic content decompression for media.
        // Keeping the downloaded bytes unchanged is important for
        // correctly resuming byte-range transfers.

        curl_easy_setopt(
            curl,
            CURLOPT_FAILONERROR,
            1L);

        char errorBuffer[CURL_ERROR_SIZE] = {};

        curl_easy_setopt(
            curl,
            CURLOPT_ERRORBUFFER,
            errorBuffer);

        bool completed = false;

        for (int attempt = 0; attempt < MAX_RETRIES; ++attempt)
        {
            errorBuffer[0] = '\0';

            // Resume only partial data created during this invocation.
            // Stale partial files are removed when the function starts.
            curl_off_t resumeOffset = 0;

            if (attempt > 0)
            {
                fsError.clear();

                if (fs::exists(temporary, fsError) && !fsError)
                {
                    const auto size =
                        fs::file_size(temporary, fsError);

                    if (!fsError)
                    {
                        resumeOffset =
                            static_cast<curl_off_t>(size);
                    }
                }
            }

            std::FILE *file =
                openOutputFile(temporary, resumeOffset > 0);

            if (!file)
            {
                error = "Cannot create partial download file: " + temporary.u8string();

                break;
            }

            DownloadContext context;
            context.file = file;
            context.resumeOffset = resumeOffset;

            curl_easy_setopt(
                curl,
                CURLOPT_RESUME_FROM_LARGE,
                resumeOffset);

            curl_easy_setopt(
                curl,
                CURLOPT_WRITEFUNCTION,
                writeToFile);

            curl_easy_setopt(
                curl,
                CURLOPT_WRITEDATA,
                &context);

            curl_easy_setopt(
                curl,
                CURLOPT_HEADERFUNCTION,
                inspectDownloadHeaders);

            curl_easy_setopt(
                curl,
                CURLOPT_HEADERDATA,
                &context);

            const CURLcode result =
                curl_easy_perform(curl);

            long httpStatus = 0;

            curl_easy_getinfo(
                curl,
                CURLINFO_RESPONSE_CODE,
                &httpStatus);

            // Flush buffered disk writes and detect local filesystem
            // failures before considering the transfer complete.
            bool fileOk = std::fflush(file) == 0;

            if (std::fclose(file) != 0)
            {
                fileOk = false;
            }

            if (!fileOk)
            {
                context.writeFailed = true;
            }

            // Some servers ignore Range and respond with HTTP 200.
            // Discard that attempt and restart the file from byte zero.
            if (context.rangeIgnored ||
                (resumeOffset > 0 && httpStatus == 200))
            {
                removeFileNoThrow(temporary);

                if (attempt + 1 < MAX_RETRIES)
                {
                    continue;
                }

                error =
                    "The server did not accept the resume request. "
                    "Please try downloading again.";

                break;
            }

            const bool success =
                result == CURLE_OK && httpStatus >= 200 && httpStatus < 300 && !context.writeFailed;

            if (success)
            {
                if (!commitDownloadedFile(
                        temporary,
                        destination,
                        error))
                {
                    break;
                }

                completed = true;
                error.clear();
                break;
            }

            if (context.writeFailed)
            {
                error =
                    "A local file write failed while downloading '" + destination.u8string() + "'. Check the available disk space and "
                                                                                             "write permissions.";

                break;
            }

            errorBuffer[CURL_ERROR_SIZE - 1] = '\0';

            if (result != CURLE_OK)
            {
                error = curlErrorMessage(result, errorBuffer);

                if (httpStatus > 0)
                {
                    error += " (HTTP " + std::to_string(httpStatus) + ")";
                }
            }
            else
            {
                error = "HTTP " + std::to_string(httpStatus);
            }

            if (attempt + 1 < MAX_RETRIES &&
                shouldRetry(result, httpStatus))
            {
                waitBeforeRetry(attempt);
                continue;
            }

            break;
        }

        curl_easy_cleanup(curl);

        if (!completed)
        {
            // Do not leave an incomplete file masquerading as a
            // successful download. Preserve any existing final file.
            removeFileNoThrow(temporary);
            return false;
        }

        return true;
    }

} // namespace ytdl
