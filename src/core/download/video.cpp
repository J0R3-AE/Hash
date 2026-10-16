
#include "ytdl/downloader.hpp"

#include "ytdl/format_selector.hpp"
#include "ytdl/http_client.hpp"
#include "ytdl/utils.hpp"

#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace ytdl
{

    void listFormats(const std::vector<Format> &formats)
    {
        if (formats.empty())
        {
            std::cout
                << "No formats were returned. The video may be unavailable, "
                   "restricted, or blocked for this client.\n";
            return;
        }

        const std::vector<std::string> entries =
            summarizeFormatEntries(formats);

        std::cout << "\nAvailable formats: ";

        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            if (i != 0)
            {
                std::cout << ", ";
            }

            std::cout << entries[i];
        }

        std::cout << '\n';
    }

    int downloadVideo(
        const std::string &title,
        const std::vector<Format> &formats,
        const std::vector<std::string> &wantedSelectors,
        const fs::path &outputDirectory)
    {
        std::string safeTitle = sanitizeFilename(title);

        if (safeTitle.empty())
        {
            safeTitle = "video";
        }

        // Validate the output directory before starting downloads.
        const fs::path destinationDirectory =
            outputDirectory.empty() ? fs::path(".") : outputDirectory;

        std::error_code fsError;

        fs::create_directories(destinationDirectory, fsError);

        if (fsError)
        {
            std::cerr
                << "Cannot create output directory '"
                << destinationDirectory.u8string()
                << "': " << fsError.message() << '\n';

            return 1;
        }

        if (!fs::is_directory(destinationDirectory, fsError) || fsError)
        {
            std::cerr
                << "Output path is not a valid directory: "
                << destinationDirectory.u8string() << '\n';

            return 1;
        }

        int failures = 0;
        int successes = 0;

        // Prevent multiple selectors from selecting the same format.
        std::set<std::string> selectedIds;

        if (wantedSelectors.empty())
        {
            std::cerr << "No format selectors were supplied.\n";
            return 1;
        }

        for (const auto &selector : wantedSelectors)
        {
            std::cout
                << "\nSelecting format "
                << selector << "...\n";

            const FormatSelection selection =
                resolveFormatSelector(
                    selector,
                    formats,
                    selectedIds);

            if (!selection.format)
            {
                std::cerr
                    << "Cannot select '"
                    << selector << "': "
                    << selection.error << '\n';

                ++failures;
                continue;
            }

            const Format &format = *selection.format;

            if (format.url.empty())
            {
                std::cerr
                    << "Format " << format.id
                    << " has no direct URL. This format may require "
                       "YouTube signature deciphering or additional "
                       "playback information, which this downloader "
                       "does not currently implement.\n";

                ++failures;
                continue;
            }

            selectedIds.insert(format.id);

            // Ensure that generated filename components are nonempty.
            std::string safeFormatId =
                sanitizeFilename(format.id);

            if (safeFormatId.empty())
            {
                safeFormatId = "unknown-format";
            }

            std::string safeExtension =
                sanitizeFilename(format.ext);

            if (safeExtension.empty())
            {
                safeExtension = "bin";
            }

            const std::string filename =
                safeTitle + "-" + safeFormatId + "." + safeExtension;

            const fs::path destination =
                destinationDirectory / fs::u8path(filename);

            std::cout
                << "Downloading format "
                << format.id
                << " (" << format.ext
                << ", " << format.resolution << ")...\n";

            std::string error;

            if (downloadUrl(format.url, destination, error))
            {
                ++successes;

                std::cout << "Download completed: "
                          << destination.u8string() << '\n';
            }
            else
            {
                ++failures;

                std::cerr
                    << "Failed to download format "
                    << format.id << ": "
                    << (error.empty()
                            ? "Unknown download error."
                            : error)
                    << '\n';

                std::cerr
                    << "Check the connection, the format URL, "
                       "and whether the URL has expired or requires "
                       "additional playback information.\n";
            }
        }

        std::cout
            << "\nDownload summary: "
            << successes << " succeeded, "
            << failures << " failed.\n";

        return failures;
    }

    int downloadFormats(
        const std::string &title,
        const std::vector<Format> &formats,
        const std::vector<std::string> &wantedSelectors,
        const fs::path &outputDirectory)
    {
        return downloadVideo(
            title,
            formats,
            wantedSelectors,
            outputDirectory);
    }

} // namespace ytdl
