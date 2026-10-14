#include "project.hpp"
#include "../platform/process.hpp"

#include <charconv>
#include <cmath>
#include <locale>
#include <limits>
#include <unordered_set>
#include <map>
#include "../platform/app_paths.hpp"
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace koralo {

namespace {
std::string esc(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '\t') out += "\\t";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

std::string unesc(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool e = false;
    for (char c : s) {
        if (!e && c == '\\') { e = true; continue; }
        if (e) {
            if (c == 't') out += '\t';
            else if (c == 'n') out += '\n';
            else out += c;
            e = false;
        } else out += c;
    }
    if (e) out += '\\';
    return out;
}

std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool e = false;
    for (char c : line) {
        if (c == '\t' && !e) { out.push_back(cur); cur.clear(); continue; }
        cur += c;
        if (c == '\\' && !e) e = true; else e = false;
    }
    out.push_back(cur);
    return out;
}

template<typename T> bool parse_num(const std::string& s,T& v){
    std::istringstream stream(s);stream.imbue(std::locale::classic());
    if(!(stream>>v))throw std::runtime_error("Invalid number: "+s);
    stream>>std::ws;if(!stream.eof())throw std::runtime_error("Trailing characters in number");
    if constexpr(std::is_floating_point_v<T>){if(!std::isfinite(v))throw std::runtime_error("Non-finite number");}
    if constexpr(std::is_integral_v<T>){if(v==std::numeric_limits<T>::max())throw std::runtime_error("Integer out of range");}
    return true;
}
char effect_value_type(const EffectValue& v){
    if(std::holds_alternative<double>(v))return 'D';
    if(std::holds_alternative<int>(v))return 'I';
    if(std::holds_alternative<bool>(v))return 'B';
    if(std::holds_alternative<std::string>(v))return 'S';
    if(std::holds_alternative<EffectColor>(v))return 'C';
    return 'V';
}

EffectValue parse_effect_value(char type,const std::string& raw){
    if(type=='D'){double v;parse_num(raw,v);return v;}
    if(type=='I'){int v;parse_num(raw,v);return v;}
    if(type=='B'){if(raw=="1"||raw=="true")return true;if(raw=="0"||raw=="false")return false;throw std::runtime_error("Invalid boolean");}
    if(type=='S')return unesc(raw);
    std::vector<double> nums;std::stringstream ss(raw);std::string part;
    while(std::getline(ss,part,',')){double value;parse_num(part,value);nums.push_back(value);}
    if(type=='C'&&nums.size()==4)return EffectColor{nums[0],nums[1],nums[2],nums[3]};
    if(type=='V'&&nums.size()==2)return EffectVec2{nums[0],nums[1]};
    throw std::runtime_error("Invalid effect parameter");
}

void write_effect(std::ostream& f,const char* scope,int owner,const EffectInstance& e){
    f<<"EFFECT\t"<<scope<<'\t'<<owner<<'\t'<<e.id<<'\t'<<esc(e.definition_id)<<'\t'<<e.enabled<<"\n";
    for(const auto&p:e.parameters){
        f<<"EPARAM\t"<<e.id<<'\t'<<esc(p.id)<<'\t'<<effect_value_type(p.value)<<'\t'<<esc(effect_value_string(p.value))<<'\t'<<p.animated<<"\n";
        for(const auto&k:p.keys)f<<"EKEY\t"<<e.id<<'\t'<<esc(p.id)<<'\t'<<k.time<<'\t'<<k.value<<'\t'<<(k.interpolation==EffectKeyInterpolation::Hold?'H':'L')<<"\n";
    }
}

EffectInstance* find_effect_instance(Project& p,int id){
    for(auto&m:p.media)for(auto&e:m.video_effects)if(e.id==id)return &e;
    for(auto&t:p.tracks){for(auto&e:t.video_effects)if(e.id==id)return &e;for(auto&e:t.audio_effects)if(e.id==id)return &e;}
    for(auto&c:p.clips){for(auto&e:c.video_effects)if(e.id==id)return &e;for(auto&e:c.audio_effects)if(e.id==id)return &e;}
    for(auto&e:p.video_output_effects)if(e.id==id)return &e;
    return nullptr;
}

}

double AnimatedDouble::value_at(double time) const {
    if (keys.empty()) return default_value;
    if (time <= keys.front().time) return keys.front().value;
    for (std::size_t i = 1; i < keys.size(); ++i) {
        const auto& a = keys[i - 1];
        const auto& b = keys[i];
        if (time <= b.time) {
            if (time >= b.time - 1e-12) return b.value;
            if (a.interpolation == KeyframeInterpolation::Hold || b.time <= a.time + 1e-12) return a.value;
            const double u = keyframe_easing(a,std::clamp((time - a.time) / (b.time - a.time), 0.0, 1.0));
            return a.value + (b.value - a.value) * u;
        }
    }
    return keys.back().value;
}

void AnimatedDouble::set_key(double time, double value, KeyframeInterpolation interpolation) {
    time = std::max(0.0, time);
    for (auto& key : keys) {
        if (std::abs(key.time - time) <= 1e-6) { key.time = time; key.value = value; key.interpolation = interpolation; return; }
    }
    keys.push_back({time, value, interpolation});
    std::sort(keys.begin(), keys.end(), [](const Keyframe& a, const Keyframe& b){ return a.time < b.time; });
}

bool AnimatedDouble::erase_key(double time, double epsilon) {
    const auto old = keys.size();
    keys.erase(std::remove_if(keys.begin(), keys.end(), [&](const Keyframe& k){ return std::abs(k.time - time) <= epsilon; }), keys.end());
    return keys.size() != old;
}

bool AnimatedDouble::has_key(double time, double epsilon) const {
    return std::any_of(keys.begin(), keys.end(), [&](const Keyframe& k){ return std::abs(k.time - time) <= epsilon; });
}

void VideoMotion::set_key(double time, double cx, double cy, double z, double degrees) {
    time = std::max(0.0, time);
    const bool empty = center_x.keys.empty() && center_y.keys.empty() && zoom.keys.empty() && rotation.keys.empty();
    if (empty && time > 1e-6) {
        center_x.set_key(0.0, center_x.default_value); center_y.set_key(0.0, center_y.default_value);
        zoom.set_key(0.0, zoom.default_value); rotation.set_key(0.0, rotation.default_value);
    }
    enabled = true;
    center_x.set_key(time, std::clamp(cx, 0.0, 1.0));
    center_y.set_key(time, std::clamp(cy, 0.0, 1.0));
    zoom.set_key(time, std::clamp(z, 1.0, 8.0));
    rotation.set_key(time, std::clamp(degrees, -360.0, 360.0));
}

void VideoMotion::erase_key(double time) {
    center_x.erase_key(time); center_y.erase_key(time); zoom.erase_key(time); rotation.erase_key(time);
    enabled = !center_x.keys.empty() || !center_y.keys.empty() || !zoom.keys.empty() || !rotation.keys.empty();
}

bool VideoMotion::has_key(double time) const {
    return center_x.has_key(time) || center_y.has_key(time) || zoom.has_key(time) || rotation.has_key(time);
}

VideoMotionSample sample_motion(const VideoMotion& motion, double event_time) {
    return {
        std::clamp(motion.center_x.value_at(event_time), 0.0, 1.0),
        std::clamp(motion.center_y.value_at(event_time), 0.0, 1.0),
        std::clamp(motion.zoom.value_at(event_time), 1.0, 8.0),
        std::clamp(motion.rotation.value_at(event_time), -360.0, 360.0)
    };
}

void split_motion(VideoMotion& left, VideoMotion& right, double split_time) {
    split_time = std::max(0.0, split_time);
    right = left;
    if (!left.enabled) return;
    const VideoMotionSample at = sample_motion(left, split_time);
    auto split_channel = [&](AnimatedDouble& l, AnimatedDouble& r, double v) {
        const auto original = l.keys;
        l.keys.clear(); r.keys.clear();
        for (const auto& key : original) {
            if (key.time < split_time - 1e-6) l.keys.push_back(key);
            else if (key.time > split_time + 1e-6) { auto shifted = key; shifted.time -= split_time; r.keys.push_back(shifted); }
        }
        l.set_key(split_time, v);
        r.set_key(0.0, v);
    };
    split_channel(left.center_x, right.center_x, at.center_x);
    split_channel(left.center_y, right.center_y, at.center_y);
    split_channel(left.zoom, right.zoom, at.zoom);
    split_channel(left.rotation, right.rotation, at.rotation);
    left.enabled = right.enabled = true;
}

Project Project::make_default() {
    Project p;
    p.add_track(TrackKind::Video, "Video 1");
    p.add_track(TrackKind::Video, "Video 2");
    p.add_track(TrackKind::Audio, "Audio 1");
    return p;
}

int Project::next_id() { return next_id_++; }
int Project::next_group_id() { return next_group_id_++; }

Track& Project::add_track(TrackKind kind, std::string track_name) {
    Track t;
    t.id = next_id();
    t.kind = kind;
    if (track_name.empty()) {
        int count = 1;
        for (auto& x : tracks) if (x.kind == kind) ++count;
        track_name = (kind == TrackKind::Video ? "Video " : "Audio ") + std::to_string(count);
    }
    t.name = std::move(track_name);
    if (kind == TrackKind::Video) {
        auto it = std::find_if(tracks.begin(), tracks.end(), [](const Track& x){ return x.kind == TrackKind::Audio; });
        if (it != tracks.end()) return *tracks.insert(it, std::move(t));
    }
    tracks.push_back(std::move(t));
    return tracks.back();
}

MediaItem& Project::add_media(MediaItem item) {
    if (item.id == 0) item.id = next_id();
    next_id_ = std::max(next_id_, item.id + 1);
    if (item.name.empty()) item.name = basename_utf8(item.path);
    media.push_back(std::move(item));
    return media.back();
}

Clip& Project::add_clip(int media_id, int track_id, double start, double in_point, double duration, int group_id) {
    Clip c;
    c.id = next_id();
    c.track_id = track_id;
    const double frame = 1.0 / std::max(1.0, settings.fps);
    c.start = std::clamp(start, 0.0, std::max(0.0, kTimelineMaxSeconds-frame));
    c.duration = std::clamp(duration, frame, std::max(frame, kTimelineMaxSeconds-c.start));
    c.group_id = group_id;
    c.takes.push_back(Take{media_id, std::max(0.0, in_point), {}});
    clips.push_back(c);
    return clips.back();
}

bool Project::add_take(int clip_id, int media_id, double offset, std::string take_name, bool make_active) {
    Clip* clip = clip_by_id(clip_id);
    if (!clip || !media_by_id(media_id)) return false;
    clip->takes.push_back(Take{media_id, std::max(0.0, offset), std::move(take_name)});
    if (make_active) clip->active_take = static_cast<int>(clip->takes.size()) - 1;
    return true;
}

bool Project::set_active_take(int clip_id, int take_index) {
    Clip* clip = clip_by_id(clip_id);
    if (!clip || take_index < 0 || take_index >= static_cast<int>(clip->takes.size())) return false;
    clip->active_take = take_index;
    return true;
}

bool Project::detach_event_group(int clip_id) {
    Clip* root = clip_by_id(clip_id);
    if (!root || root->group_id <= 0) return false;
    const int group = root->group_id;
    bool changed = false;
    for (auto& clip : clips) {
        if (clip.group_id == group) { clip.group_id = 0; changed = true; }
    }
    return changed;
}

// Track ids survive vector growth; never retain Track/Clip references across insertion.
int Project::place_media(int media_id, double at, int target_track, bool new_track) {
    const auto* media_item = media_by_id(media_id);
    if (!media_item || (!media_item->has_video && !media_item->has_audio)) return 0;
    const Track* target = track_by_id(target_track);
    if (target && target->locked) return 0;
    const TrackKind kind = target && ((target->kind == TrackKind::Video && media_item->has_video) ||
        (target->kind == TrackKind::Audio && media_item->has_audio)) ? target->kind :
        (media_item->has_video ? TrackKind::Video : TrackKind::Audio);
    if (target && target->kind != kind) { target = nullptr; new_track = true; }
    const double duration = is_still_image_path(media_item->path) ? std::max(5.0, media_item->duration) :
        (media_item->duration > 0 ? media_item->duration : 5.0);
    const auto free_track = [&](TrackKind wanted) {
        for (const auto& track : tracks) {
            if (track.kind != wanted || track.locked) continue;
            bool occupied = false;
            for (const auto& clip : clips)
                if (clip.track_id == track.id && clip.start < at + duration && clip.end() > at) { occupied = true; break; }
            if (!occupied) return track.id;
        }
        return 0;
    };
    int tid = target ? target->id : (new_track ? 0 : free_track(kind));
    if (!tid) tid = add_track(kind).id;
    int audio_tid = 0;
    if (kind == TrackKind::Video && media_item->has_audio) {
        audio_tid = free_track(TrackKind::Audio);
        if (!audio_tid) audio_tid = add_track(TrackKind::Audio).id;
    }
    const int group = audio_tid ? next_group_id() : 0;
    const int clip_id = add_clip(media_id, tid, at, 0, duration, group).id;
    if (audio_tid) add_clip(media_id, audio_tid, at, 0, duration, group);
    return clip_id;
}

bool Project::detach_audio(int clip_id) {
    const Clip* root = clip_by_id(clip_id);
    const Track* track = root ? track_by_id(root->track_id) : nullptr;
    const MediaItem* item = root ? media_by_id(root->media_id()) : nullptr;
    if (!root || !track || root->locked || track->locked || !item || !item->has_audio) return false;
    // Unlink an existing A/V group without duplicating sound or breaking video-only groups.
    if (root->group_id > 0) {
        bool video = false, audio = false;
        for (const auto* member : clips_in_group(root->group_id)) {
            const auto* owner = track_by_id(member->track_id);
            if (!owner || member->locked || owner->locked) return false;
            video |= owner->kind == TrackKind::Video;
            audio |= owner->kind == TrackKind::Audio;
        }
        if (video && audio) return detach_event_group(clip_id);
    }
    if (track->kind != TrackKind::Video) return false;
    for(const auto& existing:clips){
        const auto* owner=track_by_id(existing.track_id);
        if(owner&&owner->kind==TrackKind::Audio&&existing.media_id()==root->media_id()&&
           std::abs(existing.start-root->start)<1e-9&&std::abs(existing.duration-root->duration)<1e-9&&
           std::abs(existing.source_offset()-root->source_offset())<1e-9&&std::abs(existing.rate-root->rate)<1e-9)return false;
    }
    Clip sound = *root;
    int target = 0;
    for (const auto& candidate : tracks) {
        if (candidate.kind != TrackKind::Audio || candidate.locked) continue;
        bool occupied = false;
        for (const auto& clip : clips)
            if (clip.track_id == candidate.id && clip.start < sound.end() && clip.end() > sound.start) { occupied = true; break; }
        if (!occupied) { target = candidate.id; break; }
    }
    if (!target) target = add_track(TrackKind::Audio).id;
    sound.id = next_id(); sound.track_id = target; sound.group_id = 0;
    sound.video_effects.clear(); sound.video_motion = {}; sound.opacity = 1.0;
    clips.push_back(std::move(sound));
    return true;
}

MediaItem* Project::media_by_id(int id) { for (auto& x : media) if (x.id == id) return &x; return nullptr; }
const MediaItem* Project::media_by_id(int id) const { for (auto& x : media) if (x.id == id) return &x; return nullptr; }
Track* Project::track_by_id(int id) { for (auto& x : tracks) if (x.id == id) return &x; return nullptr; }
const Track* Project::track_by_id(int id) const { for (auto& x : tracks) if (x.id == id) return &x; return nullptr; }
Clip* Project::clip_by_id(int id) { for (auto& x : clips) if (x.id == id) return &x; return nullptr; }
const Clip* Project::clip_by_id(int id) const { for (auto& x : clips) if (x.id == id) return &x; return nullptr; }

std::vector<Clip*> Project::clips_on_track(int track_id) {
    std::vector<Clip*> out;
    for (auto& c : clips) if (c.track_id == track_id) out.push_back(&c);
    std::sort(out.begin(), out.end(), [](auto a, auto b){ return a->start < b->start; });
    return out;
}
std::vector<const Clip*> Project::clips_on_track(int track_id) const {
    std::vector<const Clip*> out;
    for (auto& c : clips) if (c.track_id == track_id) out.push_back(&c);
    std::sort(out.begin(), out.end(), [](auto a, auto b){ return a->start < b->start; });
    return out;
}
std::vector<Clip*> Project::clips_in_group(int group_id) {
    std::vector<Clip*> out;
    if (group_id <= 0) return out;
    for (auto& c : clips) if (c.group_id == group_id) out.push_back(&c);
    return out;
}
std::vector<const Clip*> Project::clips_in_group(int group_id) const {
    std::vector<const Clip*> out;
    if (group_id <= 0) return out;
    for (auto& c : clips) if (c.group_id == group_id) out.push_back(&c);
    return out;
}
std::vector<const Clip*> Project::video_at(double time) const {
    std::vector<const Clip*> out;
    for (const auto& t : tracks) {
        if (t.kind != TrackKind::Video || t.mute) continue;
        for (const auto& c : clips) if (c.track_id == t.id && !c.muted && c.contains(time)) out.push_back(&c);
    }
    return out;
}

double Project::duration() const {
    double d = 0.0;
    for (auto& c : clips) d = std::max(d, c.end());
    return d;
}

bool Project::erase_clip(int clip_id) {
    auto it = std::remove_if(clips.begin(), clips.end(), [=](const Clip& c){ return c.id == clip_id; });
    bool changed = it != clips.end();
    clips.erase(it, clips.end());
    return changed;
}

bool Project::split_clip(int clip_id, double at_time, int* right_id) {
    auto* c = clip_by_id(clip_id);
    if (!c) return false;
    const double frame = 1.0 / std::max(1.0, settings.fps);
    if (at_time <= c->start + frame || at_time >= c->end() - frame) return false;
    Clip original = *c;
    double left_duration = at_time - original.start;
    double right_duration = original.duration - left_duration;
    c->duration = left_duration;
    c->fade_in = std::min(original.fade_in, left_duration);
    c->fade_out = 0.0;
    Clip right = original;
    right.id = next_id();
    right.start = at_time;
    right.duration = right_duration;
    right.fade_in = 0.0;
    right.fade_out = std::min(original.fade_out, right_duration);
    if (original.video_motion.enabled) {
        VideoMotion left_motion = original.video_motion;
        VideoMotion right_motion;
        split_motion(left_motion, right_motion, left_duration);
        c->video_motion = std::move(left_motion);
        right.video_motion = std::move(right_motion);
    }
    // Splitting a TrackEvent duplicates all Takes.  The right event advances
    // every source offset so switching takes after a split preserves sync.
    for (auto& take : right.takes) take.offset += left_duration * original.rate;
    right.snap_offset = std::max(0.0, original.snap_offset - left_duration);
    c = clip_by_id(clip_id);
    if (c) c->snap_offset = std::min(c->snap_offset, c->duration);
    clips.push_back(right);
    if (right_id) *right_id = right.id;
    return true;
}

void Project::ripple_close(double from_time, double amount, std::optional<int> only_track) {
    if (amount <= 0) return;
    for (auto& c : clips) {
        if (c.start + 1e-9 < from_time) continue;
        if (only_track && c.track_id != *only_track) continue;
        c.start = std::max(0.0, c.start - amount);
    }
}

bool Project::save(const std::string& filename) const {
    namespace fs=std::filesystem;
    std::error_code ec;auto destination=fs::absolute(path_from_utf8(filename),ec);if(ec)return false;
    TemporaryFile temporary(destination.parent_path(),".kora");if(!temporary.valid())return false;
    std::ofstream f(temporary.path(),std::ios::binary|std::ios::trunc);f.imbue(std::locale::classic());
    auto media_path=[&](const std::string& value,bool generated=false){
        if(value.empty()||generated)return value;
        auto source=path_from_utf8(value);
        if(source.is_relative())source=(path.empty()?fs::current_path():path_from_utf8(path).parent_path())/source;
        auto relative=fs::relative(source,destination.parent_path(),ec);
        if(ec){ec.clear();return path_to_utf8(source.lexically_normal());}
        auto utf=relative.generic_u8string();return std::string(utf.begin(),utf.end());
    };
    if (!f) return false;
    f << "KORALO\t15\n";
    f << std::setprecision(12);
    f << "SETTINGS\t" << settings.width << '\t' << settings.height << '\t' << settings.fps << '\t' << settings.sample_rate << '\t'
      << (snapping ? 1 : 0) << '\t' << (auto_crossfade ? 1 : 0) << '\t' << static_cast<int>(ripple) << '\t'
      << (ignore_event_grouping ? 1 : 0) << '\t' << (slip_all_takes ? 1 : 0) << '\t' << (quantize_to_frames ? 1 : 0) << '\t'
      << settings.preview_quality << '\t' << settings.preview_fps_limit << '\t' << (settings.use_prepared_cache ? 1 : 0) << "\n";
    for (const auto& m : media) {
        f << "MEDIA\t" << m.id << '\t' << esc(media_path(m.path,m.generated_text.enabled||m.generated_vector.enabled)) << '\t' << esc(m.name) << '\t' << m.duration << '\t' << m.width << '\t' << m.height << '\t'
          << m.fps << '\t' << m.has_video << '\t' << m.has_audio << '\t' << m.audio_channels << '\t' << m.sample_rate << '\t'
          << esc(m.video_codec) << '\t' << esc(m.audio_codec) << '\t' << esc(media_path(m.proxy_path)) << "\n";
        if(m.generated_text.enabled){
            const auto& g=m.generated_text;
            f<<"GTEXT\t"<<m.id<<'\t'<<esc(g.text)<<'\t'<<esc(g.font_family)<<'\t'<<g.font_size<<'\t'<<g.bold<<'\t'<<g.italic<<'\t'<<g.alignment<<'\t'
             <<g.color_r<<'\t'<<g.color_g<<'\t'<<g.color_b<<'\t'<<g.color_a<<'\t'<<g.scale<<'\t'<<g.location_x<<'\t'<<g.location_y<<'\t'<<g.anchor<<'\t'
             <<g.outline<<'\t'<<g.outline_size<<'\t'<<g.outline_r<<'\t'<<g.outline_g<<'\t'<<g.outline_b<<'\t'<<g.outline_a<<'\t'
             <<g.shadow<<'\t'<<g.shadow_x<<'\t'<<g.shadow_y<<'\t'<<g.shadow_opacity<<'\t'<<g.animation<<"\n";
        }
        if(m.generated_vector.enabled){auto record=esc(encode_vector_scene(m.generated_vector));VectorScene checked;if(record.size()>1024*1024-32||!decode_vector_scene(encode_vector_scene(m.generated_vector),checked))return false;f<<"VANIM\t"<<m.id<<'\t'<<record<<"\n";}
        for(const auto& e:m.video_effects) write_effect(f,"MEDIA",m.id,e);
    }
    for (const auto& t : tracks) {
        f << "TRACK\t" << t.id << '\t' << (t.kind == TrackKind::Video ? 'V' : 'A') << '\t' << esc(t.name) << '\t'
          << t.mute << '\t' << t.solo << '\t' << t.locked << '\t' << t.gain << '\t' << t.pan << '\t' << t.opacity << '\t' << t.height << "\n";
        for(const auto& e:t.video_effects) write_effect(f,"TRACK",t.id,e);
        for(const auto& e:t.audio_effects) write_effect(f,"ATRACK",t.id,e);
        for(const auto& fx_name:t.imported_audio_fx) f<<"AUDIOFXREF\t"<<t.id<<'\t'<<esc(fx_name)<<"\n";
    }
    for (const auto& c : clips) {
        f << "CLIP\t" << c.id << '\t' << c.track_id << '\t' << c.group_id << '\t' << c.start << '\t' << c.duration << '\t'
          << c.rate << '\t' << c.gain << '\t' << c.opacity << '\t' << c.fade_in << '\t' << c.fade_out << '\t' << c.snap_offset << '\t'
          << c.muted << '\t' << c.locked << '\t' << c.loop << '\t' << c.active_take << "\n";
        for (std::size_t i = 0; i < c.takes.size(); ++i) {
            const auto& take = c.takes[i];
            f << "TAKE\t" << c.id << '\t' << i << '\t' << take.media_id << '\t' << take.offset << '\t' << esc(take.name) << "\n";
        }
        const auto& motion = c.video_motion;
        f << "MOTION\t" << c.id << '\t' << motion.enabled << '\t'
          << motion.center_x.default_value << '\t' << motion.center_y.default_value << '\t'
          << motion.zoom.default_value << '\t' << motion.rotation.default_value << "\n";
        auto write_keys = [&](const char* channel, const AnimatedDouble& value) {
            for (const auto& key : value.keys)
                f << "MKEY\t" << c.id << '\t' << channel << '\t' << key.time << '\t' << key.value << '\t'
                  << (key.interpolation == KeyframeInterpolation::Hold ? 'H' : 'L') << "\n";
        };
        write_keys("cx", motion.center_x); write_keys("cy", motion.center_y); write_keys("zoom", motion.zoom); write_keys("rot", motion.rotation);
        for(const auto& e:c.video_effects) write_effect(f,"EVENT",c.id,e);
        for(const auto& e:c.audio_effects) write_effect(f,"AEVENT",c.id,e);
    }
    for(const auto& e:video_output_effects) write_effect(f,"OUTPUT",0,e);
    for (const auto& m : markers) f << "MARKER\t" << m.time << '\t' << esc(m.label) << "\n";
    f.flush();if(!f)return false;f.close();if(!f)return false;
    return atomic_replace_file(temporary.path(),destination);
}

std::optional<Project> Project::load(const std::string& filename, std::string* error) {
    std::ifstream f(path_from_utf8(filename), std::ios::binary);
    if (!f) { if (error) *error = "Could not open project"; return std::nullopt; }
    Project p;
    p.media.clear(); p.tracks.clear(); p.clips.clear(); p.markers.clear();
    std::string line;
    bool header = false, have_settings=false;
    int file_version = 0;
    while (std::getline(f, line)) {
        if(line.size()>1024*1024){if(error)*error="Project record too large";return std::nullopt;}
        auto cols = split_tabs(line);
        if (cols.empty()) continue;
        if(cols[0]=="KORALO"){
            try{if(header||cols.size()!=2)throw std::runtime_error("Invalid project header");parse_num(cols[1],file_version);if(file_version<5||file_version>15)throw std::runtime_error("Unsupported project version");header=true;}
            catch(const std::exception& e){if(error)*error=e.what();return std::nullopt;}continue;
        }
        if (!header) { if (error) *error = "Not a KoraLo native project"; return std::nullopt; }
        try {
            static const std::map<std::string,std::size_t> required={{"SETTINGS",8},{"MEDIA",15},{"GTEXT",27},{"VANIM",3},{"TRACK",11},{"CLIP",15},{"TAKE",6},{"MOTION",7},{"MKEY",6},{"EFFECT",6},{"EPARAM",6},{"EKEY",6},{"MARKER",3}};
            if(cols[0]=="CLIP"&&file_version>=6&&cols.size()<16)throw std::runtime_error("Truncated event");
            const auto minimum = required.find(cols[0]);
            if (minimum != required.end() && cols.size() < minimum->second) {
                throw std::runtime_error("Truncated project record");
            }
            if (cols[0] == "SETTINGS" && cols.size() >= 8) {
                if (have_settings) {
                    throw std::runtime_error("Duplicate settings");
                }
                have_settings = true;
                parse_num(cols[1], p.settings.width); parse_num(cols[2], p.settings.height); parse_num(cols[3], p.settings.fps); parse_num(cols[4], p.settings.sample_rate);
                int v=0; parse_num(cols[5],v); p.snapping=v; parse_num(cols[6],v); p.auto_crossfade=v; parse_num(cols[7],v); p.ripple=static_cast<RippleMode>(std::clamp(v,0,2));
                if (cols.size() >= 9) { parse_num(cols[8],v); p.ignore_event_grouping=v; }
                if (cols.size() >= 10) { parse_num(cols[9],v); p.slip_all_takes=v; }
                if (cols.size() >= 11) { parse_num(cols[10],v); p.quantize_to_frames=v; }
                if (cols.size() >= 12) { parse_num(cols[11],p.settings.preview_quality); p.settings.preview_quality=std::clamp(p.settings.preview_quality,0,3); }
                if (cols.size() >= 13) { parse_num(cols[12],p.settings.preview_fps_limit); if(p.settings.preview_fps_limit<=0) p.settings.preview_fps_limit=30; }
                if (cols.size() >= 14) { parse_num(cols[13],v); p.settings.use_prepared_cache=v!=0; }
            } else if (cols[0] == "MEDIA" && cols.size() >= 15) {
                MediaItem m; int b=0;
                parse_num(cols[1],m.id);if(m.id<=0)throw std::runtime_error("Invalid media ID"); m.path=unesc(cols[2]); m.name=unesc(cols[3]); parse_num(cols[4],m.duration); parse_num(cols[5],m.width); parse_num(cols[6],m.height); parse_num(cols[7],m.fps);
                parse_num(cols[8],b); m.has_video=b; parse_num(cols[9],b); m.has_audio=b; parse_num(cols[10],m.audio_channels); parse_num(cols[11],m.sample_rate);
                m.video_codec=unesc(cols[12]); m.audio_codec=unesc(cols[13]); m.proxy_path=unesc(cols[14]); p.add_media(std::move(m));
            } else if (cols[0] == "GTEXT" && cols.size() >= 27) {
                int media_id=0,b=0;parse_num(cols[1],media_id);
                if(MediaItem* m=p.media_by_id(media_id)){
                    if (m->generated_vector.enabled) {
                        throw std::runtime_error("Conflicting generated media");
                    }
                    auto& g = m->generated_text;
                    g.enabled = true;
                    g.text = unesc(cols[2]);
                    g.font_family = unesc(cols[3]);
                    parse_num(cols[4], g.font_size);
                    parse_num(cols[5],b);g.bold=b!=0;parse_num(cols[6],b);g.italic=b!=0;parse_num(cols[7],g.alignment);
                    parse_num(cols[8],g.color_r);parse_num(cols[9],g.color_g);parse_num(cols[10],g.color_b);parse_num(cols[11],g.color_a);
                    parse_num(cols[12],g.scale);parse_num(cols[13],g.location_x);parse_num(cols[14],g.location_y);parse_num(cols[15],g.anchor);
                    parse_num(cols[16],b);g.outline=b!=0;parse_num(cols[17],g.outline_size);parse_num(cols[18],g.outline_r);parse_num(cols[19],g.outline_g);parse_num(cols[20],g.outline_b);parse_num(cols[21],g.outline_a);
                    parse_num(cols[22],b);g.shadow=b!=0;parse_num(cols[23],g.shadow_x);parse_num(cols[24],g.shadow_y);parse_num(cols[25],g.shadow_opacity);parse_num(cols[26],g.animation);
                }
            } else if (cols[0] == "VANIM" && cols.size() >= 3) {
                int id=0;parse_num(cols[1],id);auto* m=p.media_by_id(id);if(!m||m->generated_vector.enabled||m->generated_text.enabled)throw std::runtime_error("Invalid or duplicate vector owner");if(!decode_vector_scene(unesc(cols[2]),m->generated_vector,error))return std::nullopt;
            } else if (cols[0] == "TRACK" && cols.size() >= 11) {
                Track t; int b=0; parse_num(cols[1],t.id); t.kind=(cols[2]=="A"?TrackKind::Audio:TrackKind::Video); t.name=unesc(cols[3]);
                parse_num(cols[4],b);t.mute=b;parse_num(cols[5],b);t.solo=b;parse_num(cols[6],b);t.locked=b;parse_num(cols[7],t.gain);parse_num(cols[8],t.pan);parse_num(cols[9],t.opacity);parse_num(cols[10],t.height);
                p.next_id_=std::max(p.next_id_,t.id+1); p.tracks.push_back(std::move(t));
            } else if (cols[0] == "AUDIOFXREF" && cols.size() >= 3) {
                int track_id=0;parse_num(cols[1],track_id);if(Track* track=p.track_by_id(track_id))track->imported_audio_fx.push_back(unesc(cols[2]));
            } else if (cols[0] == "CLIP" && file_version >= 6 && cols.size() >= 16) {
                Clip c; int b=0;
                parse_num(cols[1],c.id);parse_num(cols[2],c.track_id);parse_num(cols[3],c.group_id);parse_num(cols[4],c.start);parse_num(cols[5],c.duration);
                parse_num(cols[6],c.rate);parse_num(cols[7],c.gain);parse_num(cols[8],c.opacity);parse_num(cols[9],c.fade_in);parse_num(cols[10],c.fade_out);parse_num(cols[11],c.snap_offset);
                parse_num(cols[12],b);c.muted=b;parse_num(cols[13],b);c.locked=b;parse_num(cols[14],b);c.loop=b;parse_num(cols[15],c.active_take);
                p.next_id_=std::max(p.next_id_,c.id+1); p.next_group_id_=std::max(p.next_group_id_,c.group_id+1); p.clips.push_back(std::move(c));
            } else if (cols[0] == "CLIP" && cols.size() >= 15) {
                // Backward-compatible v5 reader: the old Clip.media_id/in_point
                // pair becomes the first (active) Take.
                Clip c; int b=0, media_id=0; double in_point=0.0;
                parse_num(cols[1],c.id);parse_num(cols[2],media_id);parse_num(cols[3],c.track_id);parse_num(cols[4],c.group_id);parse_num(cols[5],c.start);parse_num(cols[6],c.duration);parse_num(cols[7],in_point);parse_num(cols[8],c.rate);parse_num(cols[9],c.gain);parse_num(cols[10],c.opacity);parse_num(cols[11],c.fade_in);parse_num(cols[12],c.fade_out);parse_num(cols[13],b);c.muted=b;parse_num(cols[14],b);c.locked=b;
                c.takes.push_back(Take{media_id, in_point, {}});
                p.next_id_=std::max(p.next_id_,c.id+1); p.next_group_id_=std::max(p.next_group_id_,c.group_id+1); p.clips.push_back(std::move(c));
            } else if (cols[0] == "TAKE" && cols.size() >= 6) {
                int clip_id=0,index=0,media_id=0; double offset=0.0;
                parse_num(cols[1],clip_id);parse_num(cols[2],index);parse_num(cols[3],media_id);parse_num(cols[4],offset);
                if (Clip* clip=p.clip_by_id(clip_id)) {
                    if(index<0||index>4095)throw std::runtime_error("Invalid take index");
                    if (clip->takes.size() <= static_cast<std::size_t>(index)) clip->takes.resize(static_cast<std::size_t>(index)+1);
                    clip->takes[static_cast<std::size_t>(index)] = Take{media_id,std::max(0.0,offset),unesc(cols[5])};
                }
            } else if (cols[0] == "MOTION" && cols.size() >= 7) {
                int clip_id=0, enabled=0; parse_num(cols[1],clip_id); parse_num(cols[2],enabled);
                if (Clip* clip=p.clip_by_id(clip_id)) {
                    clip->video_motion.enabled=enabled!=0;
                    parse_num(cols[3],clip->video_motion.center_x.default_value); parse_num(cols[4],clip->video_motion.center_y.default_value);
                    parse_num(cols[5],clip->video_motion.zoom.default_value); parse_num(cols[6],clip->video_motion.rotation.default_value);
                }
            } else if (cols[0] == "MKEY" && cols.size() >= 6) {
                int clip_id=0; double time=0.0,value=0.0; parse_num(cols[1],clip_id); parse_num(cols[3],time); parse_num(cols[4],value);
                if (Clip* clip=p.clip_by_id(clip_id)) {
                    AnimatedDouble* channel=nullptr;
                    if(cols[2]=="cx")channel=&clip->video_motion.center_x; else if(cols[2]=="cy")channel=&clip->video_motion.center_y;
                    else if(cols[2]=="zoom")channel=&clip->video_motion.zoom; else if(cols[2]=="rot")channel=&clip->video_motion.rotation;
                    if(channel){channel->set_key(time,value,cols[5]=="H"?KeyframeInterpolation::Hold:KeyframeInterpolation::Linear);clip->video_motion.enabled=true;}
                }
            } else if (cols[0] == "EFFECT" && cols.size() >= 6) {
                const std::string scope=cols[1];int owner=0,id=0,enabled=1;parse_num(cols[2],owner);parse_num(cols[3],id);if(id<=0||id>=2147479551)throw std::runtime_error("Invalid effect ID");parse_num(cols[5],enabled);
                const std::string definition_id=unesc(cols[4]);
                EffectInstance inst=(scope=="ATRACK"||scope=="AEVENT")?make_audio_effect_instance(definition_id,id):make_effect_instance(definition_id,id);
                inst.enabled=enabled!=0;p.next_id_=std::max(p.next_id_,id+1);
                if(scope=="MEDIA"){if(auto*m=p.media_by_id(owner))m->video_effects.push_back(std::move(inst));}
                else if(scope=="TRACK"){if(auto*t=p.track_by_id(owner))t->video_effects.push_back(std::move(inst));}
                else if(scope=="EVENT"){if(auto*c=p.clip_by_id(owner))c->video_effects.push_back(std::move(inst));}
                else if(scope=="ATRACK"){if(auto*t=p.track_by_id(owner))t->audio_effects.push_back(std::move(inst));}
                else if(scope=="AEVENT"){if(auto*c=p.clip_by_id(owner))c->audio_effects.push_back(std::move(inst));}
                else if(scope=="OUTPUT")p.video_output_effects.push_back(std::move(inst));
            } else if (cols[0] == "EPARAM" && cols.size() >= 6) {
                int id=0,animated=0;parse_num(cols[1],id);parse_num(cols[5],animated);if(auto*e=find_effect_instance(p,id)){
                    auto*param=effect_parameter(*e,unesc(cols[2]));if(!param){e->parameters.push_back({unesc(cols[2]),0.0,{},false});param=&e->parameters.back();}
                    param->value=parse_effect_value(cols[3].empty()?'D':cols[3][0],unesc(cols[4]));param->animated=animated!=0;
                }
            } else if (cols[0] == "EKEY" && cols.size() >= 6) {
                int id=0;double time=0,value=0;parse_num(cols[1],id);parse_num(cols[3],time);parse_num(cols[4],value);if(auto*e=find_effect_instance(p,id)){if(auto*param=effect_parameter(*e,unesc(cols[2])))param->set_numeric_key(time,value,cols[5]=="H"?EffectKeyInterpolation::Hold:EffectKeyInterpolation::Linear);}
            } else if (cols[0] == "MARKER" && cols.size() >= 3) {
                Marker m; parse_num(cols[1],m.time); m.label=unesc(cols[2]); p.markers.push_back(std::move(m));
            }
        } catch (...) { if (error) *error = "Malformed project"; return std::nullopt; }
    }
    namespace fs=std::filesystem;
    if(!header||!have_settings||f.bad()||p.settings.width<2||p.settings.height<2||p.settings.width>16384||p.settings.height>16384||p.settings.fps<1||p.settings.fps>240||p.settings.sample_rate<8000||p.settings.sample_rate>192000){if(error)*error="Incomplete project or invalid settings";return std::nullopt;}
    std::error_code ec;auto absolute=fs::absolute(path_from_utf8(filename),ec);if(ec){if(error)*error=ec.message();return std::nullopt;}
    p.path=path_to_utf8(absolute.lexically_normal());p.name=basename_utf8(filename);
    if(file_version>=14)for(auto& media:p.media){
        if(!media.generated_text.enabled&&!media.generated_vector.enabled&&!media.path.empty()&&path_from_utf8(media.path).is_relative())media.path=path_to_utf8((absolute.parent_path()/path_from_utf8(media.path)).lexically_normal());
        if(!media.proxy_path.empty()&&path_from_utf8(media.proxy_path).is_relative())media.proxy_path=path_to_utf8((absolute.parent_path()/path_from_utf8(media.proxy_path)).lexically_normal());
    }
    std::unordered_set<int> ids;auto unique=[&](int id){return id>0&&id<2147479551&&ids.insert(id).second;};
    for(const auto& media:p.media)if(!unique(media.id)){if(error)*error="Duplicate media ID";return std::nullopt;}
    for(const auto& track:p.tracks)if(!unique(track.id)){if(error)*error="Duplicate track ID";return std::nullopt;}
    for(const auto& clip:p.clips){if(!unique(clip.id)||!p.track_by_id(clip.track_id)||clip.duration<=0||clip.rate<=0||clip.rate>100){if(error)*error="Invalid event or track reference";return std::nullopt;}
        for(const auto& take:clip.takes)if(!p.media_by_id(take.media_id)){if(error)*error="Missing take media";return std::nullopt;}}
    if(p.tracks.empty()){p.add_track(TrackKind::Video,"Video 1");p.add_track(TrackKind::Video,"Video 2");p.add_track(TrackKind::Audio,"Audio 1");}
    for(auto& track:p.tracks){
        track.gain=std::clamp(track.gain,0.0,4.0);track.pan=std::clamp(track.pan,-1.0,1.0);track.opacity=std::clamp(track.opacity,0.0,1.0);track.height=std::clamp(track.height,40,320);
    }
    for (auto& clip : p.clips) {
        const double frame=1.0/std::max(1.0,p.settings.fps);
        clip.start=std::clamp(clip.start,0.0,std::max(0.0,kTimelineMaxSeconds-frame));
        clip.duration=std::clamp(clip.duration,frame,std::max(frame,kTimelineMaxSeconds-clip.start));
        clip.gain=std::clamp(clip.gain,0.0,4.0);clip.opacity=std::clamp(clip.opacity,0.0,1.0);clip.fade_in=std::clamp(clip.fade_in,0.0,clip.duration);clip.fade_out=std::clamp(clip.fade_out,0.0,clip.duration);clip.snap_offset=std::clamp(clip.snap_offset,0.0,clip.duration);
        auto clamp_motion=[&](AnimatedDouble& channel,double lo,double hi){channel.default_value=std::clamp(channel.default_value,lo,hi);for(auto& key:channel.keys){key.time=std::clamp(key.time,0.0,clip.duration);key.value=std::clamp(key.value,lo,hi);}std::stable_sort(channel.keys.begin(),channel.keys.end(),[](const Keyframe& a,const Keyframe& b){return a.time<b.time;});channel.keys.erase(std::unique(channel.keys.begin(),channel.keys.end(),[](const Keyframe& a,const Keyframe& b){return std::abs(a.time-b.time)<=1e-6;}),channel.keys.end());};
        clamp_motion(clip.video_motion.center_x,0.0,1.0);clamp_motion(clip.video_motion.center_y,0.0,1.0);clamp_motion(clip.video_motion.zoom,1.0,8.0);clamp_motion(clip.video_motion.rotation,-360.0,360.0);
        if (clip.takes.empty()) clip.active_take = 0;
        else clip.active_take = std::clamp(clip.active_take, 0, static_cast<int>(clip.takes.size()) - 1);
    }
    for(auto& marker:p.markers)marker.time=std::clamp(marker.time,0.0,kTimelineMaxSeconds);
    return p;
}


bool Project::collect(const std::string& directory,std::string* error)const{
    namespace fs=std::filesystem;std::error_code ec;auto target=fs::absolute(path_from_utf8(directory),ec);
    if(ec||fs::exists(target,ec)||!fs::is_directory(target.parent_path(),ec)){if(error)*error="Collection needs a new directory in an existing parent";return false;}
    TemporaryFile reserve(target.parent_path(),".collection");if(!reserve.valid()){if(error)*error="Could not reserve collection folder";return false;}
    auto staging=reserve.release();fs::remove(staging,ec);fs::create_directory(staging,ec);
    struct Cleanup{fs::path path;~Cleanup(){std::error_code ignored;fs::remove_all(path,ignored);}}cleanup{staging};
    if(ec){if(error)*error=ec.message();return false;}fs::create_directory(staging/"media",ec);if(ec){if(error)*error=ec.message();return false;}
    Project copied=*this;std::map<std::string,std::string> seen;
    auto copy_asset=[&](const std::string& raw,const std::string& asset_name)->std::string{
        auto from=path_from_utf8(raw);if(from.is_relative())from=(path.empty()?fs::current_path():path_from_utf8(path).parent_path())/from;
        from=fs::weakly_canonical(from,ec);if(ec||!fs::is_regular_file(from,ec))return{};
        auto key=path_to_utf8(from);if(seen.contains(key))return seen[key];auto output=staging/"media"/path_from_utf8(asset_name);
        if(!fs::copy_file(from,output,fs::copy_options::none,ec)||ec)return{};
        return seen[key]=path_to_utf8(output);
    };
    for(auto& media_item:copied.media){if(media_item.generated_text.enabled||media_item.generated_vector.enabled)continue;
        auto source=copy_asset(media_item.path,std::to_string(media_item.id)+"_"+basename_utf8(media_item.path));
        if(source.empty()){if(error)*error="Could not collect media: "+media_item.path;return false;}media_item.path=source;
        if(!media_item.proxy_path.empty()){ec.clear();media_item.proxy_path=copy_asset(media_item.proxy_path,std::to_string(media_item.id)+"_proxy_"+basename_utf8(media_item.proxy_path));ec.clear();}
    }
    if(!copied.save(path_to_utf8(staging/"project.kora"))){if(error)*error="Could not save collected project";return false;}
    if(fs::exists(target,ec)){if(error)*error="Collection directory already exists";return false;}
    fs::rename(staging,target,ec);if(ec){if(error)*error=ec.message();return false;}return true;
}

bool is_still_image_path(const std::string& path) {
    std::string ext = path_to_utf8(path_from_utf8(path).extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" ||
           ext == ".webp" || ext == ".tif" || ext == ".tiff" || ext == ".ppm" || ext == ".pam";
}

std::string basename_utf8(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}
std::string track_kind_name(TrackKind kind) { return kind == TrackKind::Video ? "VIDEO" : "AUDIO"; }

} // namespace koralo
