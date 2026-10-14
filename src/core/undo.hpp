#pragma once
#include "project.hpp"
#include <deque>

namespace koralo {

class UndoStack {
public:
    explicit UndoStack(std::size_t limit = 100) : limit_(limit) {}

    void begin(const Project& p) {
        if (!pending_) pending_ = p;
    }
    bool commit(const Project& p) {
        if (!pending_) return false;
        bool changed = !equivalent(*pending_, p);
        if (changed) {
            undo_.push_back(std::move(*pending_));
            if (undo_.size() > limit_) undo_.pop_front();
            redo_.clear();
        }
        pending_.reset();
        return changed;
    }
    void cancel() { pending_.reset(); }
    bool undo(Project& p) {
        pending_.reset();
        if (undo_.empty()) return false;
        redo_.push_back(p);
        p = std::move(undo_.back()); undo_.pop_back(); return true;
    }
    bool redo(Project& p) {
        pending_.reset();
        if (redo_.empty()) return false;
        undo_.push_back(p);
        p = std::move(redo_.back()); redo_.pop_back(); return true;
    }
    void clear() { undo_.clear(); redo_.clear(); pending_.reset(); }
    bool can_undo() const { return !undo_.empty(); }
    bool can_redo() const { return !redo_.empty(); }
private:
    static bool equivalent(const Project& a, const Project& b) {
        auto same_effects=[](const std::vector<EffectInstance>& x,const std::vector<EffectInstance>& y){
            if(x.size()!=y.size())return false;
            for(std::size_t i=0;i<x.size();++i){
                if(x[i].id!=y[i].id||x[i].definition_id!=y[i].definition_id||x[i].enabled!=y[i].enabled||x[i].parameters.size()!=y[i].parameters.size())return false;
                for(std::size_t j=0;j<x[i].parameters.size();++j){const auto&xp=x[i].parameters[j];const auto&yp=y[i].parameters[j];
                    if(xp.id!=yp.id||xp.value!=yp.value||xp.animated!=yp.animated||xp.keys.size()!=yp.keys.size())return false;
                    for(std::size_t k=0;k<xp.keys.size();++k)if(xp.keys[k].time!=yp.keys[k].time||xp.keys[k].value!=yp.keys[k].value||xp.keys[k].interpolation!=yp.keys[k].interpolation)return false;
                }
            }
            return true;
        };
        if (a.settings.width!=b.settings.width || a.settings.height!=b.settings.height || a.settings.fps!=b.settings.fps ||
            a.settings.sample_rate!=b.settings.sample_rate || a.settings.preview_quality!=b.settings.preview_quality || a.settings.preview_fps_limit!=b.settings.preview_fps_limit || a.settings.use_prepared_cache!=b.settings.use_prepared_cache || a.snapping!=b.snapping || a.quantize_to_frames!=b.quantize_to_frames || a.auto_crossfade!=b.auto_crossfade ||
            a.ignore_event_grouping!=b.ignore_event_grouping || a.slip_all_takes!=b.slip_all_takes ||
            a.ripple!=b.ripple || a.clips.size()!=b.clips.size() || a.tracks.size()!=b.tracks.size() ||
            a.media.size()!=b.media.size() || a.markers.size()!=b.markers.size() || !same_effects(a.video_output_effects,b.video_output_effects)) return false;
        for (std::size_t i=0;i<a.tracks.size();++i) {
            const auto& x=a.tracks[i]; const auto& y=b.tracks[i];
            if (x.id!=y.id || x.kind!=y.kind || x.name!=y.name || x.mute!=y.mute || x.solo!=y.solo || x.locked!=y.locked ||
                x.gain!=y.gain || x.pan!=y.pan || x.opacity!=y.opacity || x.height!=y.height || x.imported_audio_fx!=y.imported_audio_fx || !same_effects(x.video_effects,y.video_effects) || !same_effects(x.audio_effects,y.audio_effects)) return false;
        }
        for (std::size_t i=0;i<a.media.size();++i) {
            const auto& x=a.media[i];const auto& y=b.media[i];
            if(x.id!=y.id||x.path!=y.path||x.name!=y.name||!(x.generated_text==y.generated_text)||encode_vector_scene(x.generated_vector)!=encode_vector_scene(y.generated_vector)||!same_effects(x.video_effects,y.video_effects))return false;
        }
        for (std::size_t i=0;i<a.clips.size();++i) {
            const auto& x=a.clips[i]; const auto& y=b.clips[i];
            if (x.id!=y.id || x.track_id!=y.track_id || x.group_id!=y.group_id ||
                x.start!=y.start || x.duration!=y.duration || x.rate!=y.rate ||
                x.gain!=y.gain || x.opacity!=y.opacity || x.fade_in!=y.fade_in || x.fade_out!=y.fade_out ||
                x.snap_offset!=y.snap_offset || x.muted!=y.muted || x.locked!=y.locked || x.loop!=y.loop ||
                x.active_take!=y.active_take || x.takes.size()!=y.takes.size() || x.video_motion.enabled!=y.video_motion.enabled || !same_effects(x.video_effects,y.video_effects) || !same_effects(x.audio_effects,y.audio_effects)) return false;
            auto same_anim=[](const AnimatedDouble& u,const AnimatedDouble& v){
                if(u.default_value!=v.default_value||u.keys.size()!=v.keys.size())return false;
                for(std::size_t k=0;k<u.keys.size();++k)if(u.keys[k].time!=v.keys[k].time||u.keys[k].value!=v.keys[k].value||u.keys[k].interpolation!=v.keys[k].interpolation)return false;
                return true;
            };
            if(!same_anim(x.video_motion.center_x,y.video_motion.center_x)||!same_anim(x.video_motion.center_y,y.video_motion.center_y)||
               !same_anim(x.video_motion.zoom,y.video_motion.zoom)||!same_anim(x.video_motion.rotation,y.video_motion.rotation))return false;
            for (std::size_t j=0;j<x.takes.size();++j) {
                if (x.takes[j].media_id!=y.takes[j].media_id || x.takes[j].offset!=y.takes[j].offset || x.takes[j].name!=y.takes[j].name) return false;
            }
        }
        for (std::size_t i=0;i<a.markers.size();++i) {
            if (a.markers[i].time!=b.markers[i].time || a.markers[i].label!=b.markers[i].label) return false;
        }
        return true;
    }
    std::size_t limit_;
    std::deque<Project> undo_, redo_;
    std::optional<Project> pending_;
};

} // namespace koralo
