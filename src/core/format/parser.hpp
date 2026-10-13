#pragma once

#include "ytdl/types.hpp"

#include <boost/property_tree/ptree.hpp>

#include <vector>

namespace ytdl {

// Parse the streamingData section from a YouTube player response.
std::vector<Format> collectFormats(const boost::property_tree::ptree& playerResponse);

} // namespace ytdl
