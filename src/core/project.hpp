#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "effects.hpp"
#include "vector_animation.hpp"

namespace koralo {

inline constexpr double kTimelineMaxSeconds = 12.0 * 60.0 * 60.0; // 12 hours

enum class TrackKind { Video, Audio };

enum class RippleMode { Off, AffectedTrack, AllTracks };

struct ProjectSettings {
    int width = 1920;
    int height = 1080;
    double fps = 30.0;
    int sample_rate = 48000;
    int preview_quality = 1; // 0 Draft, 1 Preview, 2 Good, 3 Best
    int preview_fps_limit = 30;
    bool use_prepared_cache = true;
};


struct GeneratedText {
    bool operator==(const GeneratedText&)const=default;
    bool enabled = false;
    std::string text = "Sample Text";
    std::string font_family = "Inter";
    double font_size = 48.0;
    bool bold = false;
    bool italic = false;
    int alignment = 1; // 0 left, 1 center, 2 right
    double color_r = 1.0, color_g = 1.0, color_b = 1.0, color_a = 1.0;
    double scale = 1.0;
    double location_x = 0.5, location_y = 0.5;
    int anchor = 4; // 3x3 grid, center = 4
    bool outline = false;
    double outline_size = 2.0;
    double outline_r = 0.0, outline_g = 0.0, outline_b = 0.0, outline_a = 1.0;
    bool shadow = false;
    double shadow_x = 4.0, shadow_y = 4.0, shadow_opacity = 0.55;
    int animation = 0; // 0 none, 1 fade
};

struct MediaItem {
    int id = 0;
    std::string path;
    std::string name;
    double duration = 0.0;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    bool has_video = false;
    bool has_audio = false;
    int audio_channels = 0;
    int sample_rate = 0;
    std::string video_codec;
    std::string audio_codec;
    std::string proxy_path;
    GeneratedText generated_text;
    VectorScene generated_vector;
    std::vector<EffectInstance> video_effects; // Media FX
};

struct Track {
    int id = 0;
    TrackKind kind = TrackKind::Video;
    std::string name;
    bool mute = false;
    bool solo = false;
    bool locked = false;
    double gain = 1.0;
    double pan = 0.0;
    double opacity = 1.0;
    int height = 76;
    std::vector<EffectInstance> video_effects; // Track FX
    std::vector<EffectInstance> audio_effects; // Native Audio FX
    std::vector<std::string> imported_audio_fx; // Opaque clean-room identities from imported projects; DSP mapping may be pending.
};

// A Take is source media attached to a timeline event.  This mirrors the
// public VEGAS object-model separation (Media -> Take -> TrackEvent) without
// depending on any VEGAS implementation detail.  Timeline placement belongs
// to Clip/TrackEvent; source selection and source offset belong to Take.
struct Take {
    int media_id = 0;
    double offset = 0.0;
    std::string name;
};

// Event-local spatial animation. Center values are normalized to [0, 1].
// zoom=1 means the full project frame; larger values zoom in.  Keyframe
// times are relative to the TrackEvent start, not source-media time.
struct VideoMotion {
    AnimatedDouble center_x{0.5,{}};
    AnimatedDouble center_y{0.5,{}};
    AnimatedDouble zoom{1.0,{}};
    AnimatedDouble rotation{0.0,{}};
    bool enabled = false;

    void set_key(double time, double cx, double cy, double z, double degrees);
    void erase_key(double time);
    [[nodiscard]] bool has_key(double time) const;
};

struct VideoMotionSample {
    double center_x = 0.5;
    double center_y = 0.5;
    double zoom = 1.0;
    double rotation = 0.0;
};

[[nodiscard]] VideoMotionSample sample_motion(const VideoMotion& motion, double event_time);
void split_motion(VideoMotion& left, VideoMotion& right, double split_time);

struct TrackEvent {
    int id = 0;
    int track_id = 0;
    int group_id = 0;
    double start = 0.0;
    double duration = 1.0;
    double rate = 1.0;
    double gain = 1.0;
    double opacity = 1.0;
    double fade_in = 0.0;
    double fade_out = 0.0;
    double snap_offset = 0.0;
    bool muted = false;
    bool locked = false;
    bool loop = false;
    VideoMotion video_motion;
    std::vector<EffectInstance> video_effects; // Event FX
    std::vector<EffectInstance> audio_effects; // Event Audio FX
    std::vector<Take> takes;
    int active_take = 0;

    [[nodiscard]] double end() const { return start + duration; }
    [[nodiscard]] bool contains(double t) const { return t >= start && t < end(); }
    [[nodiscard]] const Take* active_take_ptr() const {
        return active_take >= 0 && active_take < static_cast<int>(takes.size()) ? &takes[static_cast<std::size_t>(active_take)] : nullptr;
    }
    [[nodiscard]] Take* active_take_ptr() {
        return active_take >= 0 && active_take < static_cast<int>(takes.size()) ? &takes[static_cast<std::size_t>(active_take)] : nullptr;
    }
    [[nodiscard]] int media_id() const {
        if (const Take* take = active_take_ptr()) return take->media_id;
        return 0;
    }
    [[nodiscard]] double source_offset() const {
        if (const Take* take = active_take_ptr()) return take->offset;
        return 0.0;
    }
    void set_source_offset(double value) {
        if (Take* take = active_take_ptr()) take->offset = std::max(0.0, value);
    }
};

// Transitional source-compatibility alias.  New code should prefer
// TrackEvent; the existing UI/EditController still uses Clip while the model
// migration is completed incrementally.
using Clip = TrackEvent;

struct Marker {
    double time = 0.0;
    std::string label;
};

class Project {
public:
    ProjectSettings settings;
    std::string path;
    std::string name = "Untitled.kora";
    std::vector<MediaItem> media;
    std::vector<Track> tracks;
    std::vector<Clip> clips;
    std::vector<Marker> markers;
    std::vector<EffectInstance> video_output_effects; // Video Output FX
    bool snapping = true;
    bool quantize_to_frames = true;
    bool auto_crossfade = true;
    bool ignore_event_grouping = false;
    bool slip_all_takes = false;
    RippleMode ripple = RippleMode::Off;

    static Project make_default();

    int next_id();
    int next_group_id();
    Track& add_track(TrackKind kind, std::string track_name = {});
    MediaItem& add_media(MediaItem item);
    Clip& add_clip(int media_id, int track_id, double start, double in_point, double duration, int group_id = 0);
    bool add_take(int clip_id, int media_id, double offset = 0.0, std::string take_name = {}, bool make_active = false);
    bool set_active_take(int clip_id, int take_index);
    bool detach_event_group(int clip_id);
    bool detach_audio(int clip_id);
    int place_media(int media_id, double at, int target_track = 0, bool new_track = false);

    MediaItem* media_by_id(int id);
    const MediaItem* media_by_id(int id) const;
    Track* track_by_id(int id);
    const Track* track_by_id(int id) const;
    Clip* clip_by_id(int id);
    const Clip* clip_by_id(int id) const;

    std::vector<Clip*> clips_on_track(int track_id);
    std::vector<const Clip*> clips_on_track(int track_id) const;
    std::vector<Clip*> clips_in_group(int group_id);
    std::vector<const Clip*> clips_in_group(int group_id) const;
    std::vector<const Clip*> video_at(double time) const;
    double duration() const;

    bool erase_clip(int clip_id);
    bool split_clip(int clip_id, double at_time, int* right_id = nullptr);
    void ripple_close(double from_time, double amount, std::optional<int> only_track = std::nullopt);

    bool save(const std::string& filename) const;
    bool collect(const std::string& directory,std::string* error=nullptr)const;
    static std::optional<Project> load(const std::string& filename, std::string* error = nullptr);

private:
    int next_id_ = 1;
    int next_group_id_ = 1;
};

bool is_still_image_path(const std::string& path);
std::string basename_utf8(const std::string& path);
std::string track_kind_name(TrackKind kind);

} // namespace koralo
