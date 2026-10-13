#pragma once

#include "ytdl/types.hpp"

#include <set>
#include <string>
#include <vector>

namespace ytdl {

struct FormatSelection {
    const Format* format = nullptr;
    std::string error;
};

// Resolve either an exact numeric YouTube format ID or a container extension
// such as "mp4". Repeating extensions can select additional distinct formats.
FormatSelection resolveFormatSelector(
    const std::string& selector,
    const std::vector<Format>& formats,
    const std::set<std::string>& alreadySelectedIds);

} // namespace ytdl
