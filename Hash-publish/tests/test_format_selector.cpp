#include "ytdl/format_selector.hpp"

#include <cassert>
#include <set>
#include <string>
#include <vector>

int main()
{
    using ytdl::Format;
    using ytdl::resolveFormatSelector;

    const std::vector<Format> formats = {
        {"18", "mp4", "640x360", "600", "30", "avc1", "mp4a", "", "https://test/18"},
        {"22", "mp4", "1280x720", "2000", "30", "avc1", "mp4a", "", "https://test/22"},
        {"137", "mp4", "1920x1080", "4000", "30", "avc1", "none", "", "https://test/137"},
        {"140", "m4a", "audio only", "128", "N/A", "none", "mp4a", "", "https://test/140"},
        {"251", "webm", "audio only", "160", "N/A", "none", "opus", "", "https://test/251"},
        {"99", "webm", "1280x720", "1500", "30", "vp9", "opus", "", ""}
    };

    const std::set<std::string> none;
    auto bestMp4 = resolveFormatSelector("mp4", formats, none);
    assert(bestMp4.format && bestMp4.format->id == "22");

    const std::set<std::string> selectedBest{"22"};
    auto secondMp4 = resolveFormatSelector(".MP4", formats, selectedBest);
    assert(secondMp4.format && secondMp4.format->id == "18");

    auto exactId = resolveFormatSelector("18", formats, none);
    assert(exactId.format && exactId.format->id == "18");

    auto audio = resolveFormatSelector("m4a", formats, none);
    assert(audio.format && audio.format->id == "140");

    auto qualityMp4 = resolveFormatSelector("mp4:1080", formats, none);
    assert(qualityMp4.format && qualityMp4.format->id == "137");

    auto mp3 = resolveFormatSelector("mp3", formats, none);
    assert(!mp3.format && mp3.error.find("does not normally provide MP3") != std::string::npos);

    const std::set<std::string> bothMp4Selected{"18", "22"};
    auto exhaustedMp4 = resolveFormatSelector("mp4", formats, bothMp4Selected);
    assert(!exhaustedMp4.format && exhaustedMp4.error.find("No additional") != std::string::npos);

    return 0;
}
