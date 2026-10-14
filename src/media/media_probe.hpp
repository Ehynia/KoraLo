#pragma once

#include "../core/project.hpp"

#include <optional>
#include <string>

namespace koralo {

// FFprobe-backed metadata boundary. Keeping import metadata parsing outside
// MediaEngine lets playback/render scheduling evolve independently.
std::optional<MediaItem> probe_media_file(const std::string& ffprobe_path,
                                          const std::string& path,
                                          std::string* error = nullptr);

} // namespace koralo
