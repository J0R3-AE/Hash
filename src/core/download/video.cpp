#include "ytdl/downloader.hpp"

#include "ytdl/format_selector.hpp"
#include "ytdl/http_client.hpp"
#include "ytdl/utils.hpp"

#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace ytdl {

void listFormats(const std::vector<Format>& formats)
{
    if (formats.empty()) {
        std::cout << "No formats were returned. The video may be unavailable, "
                     "restricted, or blocked for this client.\n";
        return;
    }

    const std::vector<std::string> entries = summarizeFormatEntries(formats);

    std::cout << "\nAvailable formats: ";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i > 0)
            std::cout << ", ";
        std::cout << entries[i];
    }
    std::cout << '\n';
}

int downloadVideo(const std::string& title,
                  const std::vector<Format>& formats,
                  const std::vector<std::string>& wantedSelectors,
                  const fs::path& outputDirectory)
{
    const std::string safeTitle = sanitizeFilename(title);
    int failures = 0;
    std::set<std::string> selectedIds;

    for (const auto& selector : wantedSelectors) {
        std::cout << "\nSelecting format " << selector << "...\n";
        const FormatSelection selection =
            resolveFormatSelector(selector, formats, selectedIds);

        if (!selection.format) {
            std::cerr << "Cannot select '" << selector << "': "
                      << selection.error << '\n';
            ++failures;
            continue;
        }

        const Format& format = *selection.format;
        if (format.url.empty()) {
            std::cerr << "Format " << format.id << " has no direct URL. YouTube returned "
                         "a protected format; this program does not yet implement "
                         "the changing JavaScript signature-deciphering logic.\n";
            ++failures;
            continue;
        }

        selectedIds.insert(format.id);

        const fs::path destination = outputDirectory /
            (safeTitle + "-" + sanitizeFilename(format.id) + "." +
             sanitizeFilename(format.ext));

        std::cout << "Downloading format " << format.id << " (" << format.ext
                  << ", " << format.resolution << ")...\n";

        std::string error;
        if (downloadUrl(format.url, destination, error)) {
            std::cout << "Download completed: " << destination.string() << '\n';
        } else {
            std::cerr << "Failed to download format " << format.id << ": " << error
                      << "\nThe direct URL may have expired or require an "
                         "additional YouTube playback token.\n";
            ++failures;
        }
    }

    return failures;
}

int downloadFormats(const std::string& title,
                    const std::vector<Format>& formats,
                    const std::vector<std::string>& wantedSelectors,
                    const fs::path& outputDirectory)
{
    return downloadVideo(title, formats, wantedSelectors, outputDirectory);
}

} // namespace ytdl
