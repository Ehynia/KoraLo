#include "../platform/process.hpp"
#include <filesystem>
#include <cmath>
#include <algorithm>
#include "playback_controller.hpp"

#include <algorithm>
#include <utility>

namespace koralo {

std::vector<PreviewRequest> build_preview_layers(const Project& project,double time,bool prepared_cache){
    std::vector<PreviewRequest> layers;
    bool solo=false;for(const auto& track:project.tracks)if(track.kind==TrackKind::Video&&track.solo)solo=true;
    for(const Clip* clip:project.video_at(time)){
        const Track* track=project.track_by_id(clip->track_id);
        const MediaItem* media=project.media_by_id(clip->media_id());
        if(!track||!media||!media->has_video||(solo&&!track->solo))continue;
        PreviewRequest layer;layer.stream_id=clip->id;layer.preserve_alpha=true;
        layer.effect_time=std::clamp(time-clip->start,0.0,clip->duration);
        layer.time=clip->source_offset()+layer.effect_time*clip->rate;
        if(clip->loop&&media->duration>0)layer.time=std::fmod(layer.time,media->duration);
        layer.time=std::clamp(layer.time,0.0,std::max(0.0,media->duration-1.0/std::max(1.0,media->fps)));
        layer.source_rate=clip->rate;
        const bool proxy=!media->proxy_path.empty()&&std::filesystem::exists(path_from_utf8(media->proxy_path));
        layer.path=proxy?media->proxy_path:media->path;layer.allow_prepared_cache=prepared_cache&&!proxy;
        double fade_in=clip->fade_in,fade_out=clip->fade_out;
        if(project.auto_crossfade)for(const auto* other:project.clips_on_track(clip->track_id)){
            if(other->id==clip->id)continue;
            if(other->start<=clip->start&&other->end()>clip->start)fade_in=std::max(fade_in,std::min(clip->duration,other->end()-clip->start));
            if(other->start>clip->start&&other->start<clip->end())fade_out=std::max(fade_out,std::min(clip->duration,clip->end()-other->start));
        }
        double fade=1.0;
        if(fade_in>1e-9)fade*=std::clamp(layer.effect_time/fade_in,0.0,1.0);
        if(fade_out>1e-9)fade*=std::clamp((clip->duration-layer.effect_time)/fade_out,0.0,1.0);
        layer.opacity=std::clamp(clip->opacity*track->opacity*fade,0.0,1.0);
        if(clip->video_motion.enabled){layer.apply_motion=true;layer.motion=sample_motion(clip->video_motion,layer.effect_time);}
        layer.effects=media->video_effects;
        layer.effects.insert(layer.effects.end(),clip->video_effects.begin(),clip->video_effects.end());
        layer.effects.insert(layer.effects.end(),track->video_effects.begin(),track->video_effects.end());
        if(media->generated_vector.enabled){layer.path.clear();layer.vector_source=media->generated_vector;}
        if(media->generated_text.enabled){
            layer.path.clear();layer.has_text_overlay=true;layer.text_overlay=media->generated_text;
            layer.text_event_time=layer.effect_time;layer.text_event_duration=clip->duration;
        }
        layers.push_back(std::move(layer));
    }
    return layers;
}


void PlaybackController::note_request(const PreviewRequest& request) {
    std::lock_guard lock(mutex_);
    const auto epoch = request.playback ? request.playback_epoch : 0;
    if (!epoch || epoch != latest_playback_epoch_.load(std::memory_order_relaxed)) {
        results_.clear();
    }
    latest_playback_epoch_.store(epoch, std::memory_order_relaxed);
    latest_preview_token_.store(
        std::max(latest_preview_token_.load(std::memory_order_relaxed), request.token),
        std::memory_order_release);
}

void PlaybackController::queue_request(PreviewRequest request, std::string key) {
    {
        std::lock_guard lock(mutex_);
        if (pending_ && pending_key_ == key) {
            // Pixels may match across pause/seek, but scheduling metadata must
            // belong to the newest request (epoch, playback and stream mode).
            pending_ = std::move(request);
            return;
        }
        pending_ = std::move(request);
        pending_key_ = std::move(key);
    }
    cv_.notify_one();
}

bool PlaybackController::wait_next(const std::atomic<bool>& stop, PreviewRequest& request) {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] { return stop.load() || pending_.has_value(); });
    if (stop.load()) return false;
    request = std::move(*pending_);
    pending_.reset();
    pending_key_.clear();
    return true;
}

void PlaybackController::wake() { cv_.notify_all(); }

bool PlaybackController::should_publish(const PreviewRequest& request) const {
    const bool stale = request.token < latest_preview_token_.load(std::memory_order_acquire);
    const bool advancing = request.playback && request.playback_epoch != 0 &&
        request.playback_epoch == latest_playback_epoch_.load(std::memory_order_relaxed);
    return !stale || advancing;
}

void PlaybackController::publish_worker_result(const PreviewRequest& request,
                                               Frame frame,
                                               std::string error) {
    std::lock_guard lock(mutex_);
    const bool still_current =
        request.token >= latest_preview_token_.load(std::memory_order_acquire) ||
        (request.playback && request.playback_epoch != 0 &&
         request.playback_epoch == latest_playback_epoch_.load(std::memory_order_relaxed));
    if (!still_current) return;
    results_.push_back({request.token, std::move(frame), std::move(error)});
    if (results_.size() > 4) results_.pop_front();
}

void PlaybackController::publish_cached(std::uint64_t token, Frame frame) {
    std::lock_guard lock(mutex_);
    results_.push_back({token, std::move(frame), {}});
}

bool PlaybackController::poll(std::uint64_t& token, Frame& frame, std::string& error) {
    std::lock_guard lock(mutex_);
    if (results_.empty()) return false;
    auto newest = std::max_element(results_.begin(), results_.end(),
        [](const Result& a, const Result& b) { return a.token < b.token; });
    Result result = std::move(*newest);
    results_.clear();
    token = result.token;
    frame = std::move(result.frame);
    error = std::move(result.error);
    return true;
}

} // namespace koralo
