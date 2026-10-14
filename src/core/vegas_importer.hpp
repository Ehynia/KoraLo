#pragma once
#include "project.hpp"
#include <optional>
#include <string>
#include <vector>

namespace koralo {

struct VegasImportedChromaKey {
    bool valid = false;
    EffectColor color{0.0,1.0,0.0,1.0};
    double low_threshold = 0.08;
    double high_threshold = 0.32;
    double blur_amount = 0.0;
    bool show_mask_only = false;
};

struct VegasImportedEvent {
    double start = 0.0;
    double duration = 0.0;
    double source_offset = 0.0;
    std::string media_hint;
    bool generated_media = false;
    std::vector<std::string> video_fx_ids;
    // Validated old Svfx Chroma Keyer fields. Other private parameter blobs
    // remain opaque until differential samples establish their schema.
    VegasImportedChromaKey chroma_key;
};

struct VegasImportedTrack {
    int raw_kind = -1;
    std::vector<VegasImportedEvent> events;
    std::vector<std::string> audio_fx_names;
};

// Clean-room reader for the observed outer VEGAS Wave64 container.  Fields are
// decoded only when their record shape and units have been validated against
// supplied VEG projects. Unknown records remain opaque.
struct VegasImportInspection {
    ProjectSettings settings{};
    bool has_settings = false;
    std::vector<std::string> media_paths;
    std::vector<VegasImportedTrack> tracks;
    std::vector<std::string> observed_video_fx_ids;
    std::vector<std::string> observed_audio_fx_names;
    std::vector<std::string> observed_generators;
    std::vector<std::string> warnings;
};

std::optional<VegasImportInspection> inspect_vegas_project(const std::string& filename,
                                                            std::string* error = nullptr);
Project make_vegas_import_project(const VegasImportInspection& inspection,
                                  const std::string& source_filename);

} // namespace koralo
