#pragma once

#include "../core/project.hpp"
#include "../platform/process.hpp"
#include "frame_cache.hpp"
#include "ffmpeg_runtime.hpp"
#include "render_planner.hpp"
#include "render_control.hpp"
#include "playback_controller.hpp"
#include "decoder_pool.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace koralo {

struct AudioMeterLevel {
    double left = 0.0;
    double right = 0.0;
    bool ready = false;
};

struct MediaPreparationProgress {
    double progress = 0.0;
    std::string stage = "Idle";
    bool queued = false;
    bool running = false;
    bool ready = false;
    bool failed = false;
    bool proxy_ready = false;
    std::string proxy_path;
};

class MediaEngine {
public:
    MediaEngine();
    ~MediaEngine();
    MediaEngine(const MediaEngine&) = delete;
    MediaEngine& operator=(const MediaEngine&) = delete;

    RenderControlSnapshot render_control_status() const { return render_control_snapshot(render_control_live_); }
    bool ready() const { return !ffmpeg_.empty()&&!ffprobe_.empty(); }
    const std::string& ffmpeg_path() const { return ffmpeg_; }
    const std::string& ffprobe_path() const { return ffprobe_; }
    const std::string& ffplay_path() const { return ffplay_; }
    bool audio_preview_ready() const { return !ffplay_.empty(); }
    std::string diagnostics() const;
    const GpuBackendInfo& gpu_backend() const { return gpu_; }
    void set_gpu_enabled(bool enabled) { gpu_enabled_flag_.store(enabled); clear_cache(); }
    bool gpu_enabled() const { return gpu_enabled_flag_.load(); }

    // Timeline thumbnails are decoded on their own worker so event filmstrips
    // never block the main preview queue. A missing return value means the
    // thumbnail was queued and will appear on a later paint.
    std::optional<Frame> timeline_thumbnail(const std::string& path,double time,int width,int height);
    std::optional<Frame> vector_thumbnail(const VectorScene&,double time,int width,int height);
    std::uint64_t thumbnail_revision() const { return thumbnail_revision_.load(); }
    std::uint64_t preview_stream_start_count() const { return stream_start_count_.load(); }
    std::uint64_t source_decode_count() const { return source_decode_count_.load(); }

    std::optional<MediaItem> probe(const std::string& path, std::string* error = nullptr) const;
    void request_preview(PreviewRequest req);
    bool poll_preview(std::uint64_t& token, Frame& frame, std::string& error);
    void clear_cache();
    void clear_composite_cache();

    // A single ffplay lavfi graph combines all audible tracks on one output
    // clock. Audio device master-clock feedback remains future work.
    bool sync_audio_preview(const Project& project, double playhead, std::string* error = nullptr);
    void stop_audio_preview();

    // Records the default system input as headerless PCM while capture is
    // active; stop_audio_recording wraps the bytes into a standard PCM WAV so
    // an abrupt helper-process stop can never leave a corrupt WAV header.
    bool start_audio_recording(const std::string& raw_path,int sample_rate=48000,int channels=1,std::string* error=nullptr);
    bool stop_audio_recording(const std::string& wav_path,double* duration=nullptr,std::string* error=nullptr);
    bool audio_recording_active();
    static bool finalize_pcm16le_recording(const std::string& raw_path,const std::string& wav_path,int sample_rate,int channels,double* duration=nullptr,std::string* error=nullptr);

    // Cached source-envelope based master meter. Missing source envelopes are
    // analyzed asynchronously, so querying this from the UI never launches
    // FFmpeg on the paint/tick thread.
    AudioMeterLevel master_meter(const Project& project, double playhead);

    // Import/render preflight. Queues representative frames and the audio
    // envelope without blocking the UI so later scrubbing/render setup starts
    // from warm caches.
    void prepare_media(const MediaItem& media);
    void prepare_project(const Project& project);
    MediaPreparationProgress preparation_status(const std::string& path) const;
    MediaPreparationProgress overall_preparation_status() const;

    bool create_proxy(const MediaItem& media, const std::string& output, int height, std::string* error = nullptr) const;
    bool render_project(const Project& p, const std::string& output, std::atomic<double>* progress = nullptr, std::string* error = nullptr, const RenderOptions* options = nullptr,const std::atomic<bool>* cancel=nullptr) const;

    static std::string locate_binary(const std::string& name);

private:
    mutable RenderControlLive render_control_live_;
    bool render_fsr_project(const Project&,const std::string&,std::atomic<double>*,std::string*,const RenderOptions&,const std::atomic<bool>*)const;
    bool render_native_project(const Project&,const std::string&,std::atomic<double>*,std::string*,const RenderOptions*,const std::atomic<bool>*)const;
    void worker_loop();
    void meter_worker_loop();
    void thumbnail_worker_loop();
    void preparation_worker_loop();
    std::string prepared_preview_path(const std::string& original) const;
    Frame decode_frame(const PreviewRequest& req, std::string* error) const;
    Frame decode_stream_frame(const PreviewRequest& req, std::string* error);
    Frame decode_stream_source(const PreviewRequest& req, DecoderPool& decoder, std::string* error);
    Frame decode_layers(const PreviewRequest& req, bool streaming, std::string* error);
    bool start_preview_stream(DecoderPool& decoder, const PreviewRequest& req,const std::string& input_path,bool hardware,std::string* error);
    void reset_preview_stream();
    static void apply_preview_opacity(Frame& frame,double opacity);
    static void apply_preview_pipeline(Frame& frame,const PreviewRequest& req);
    std::string frame_key(const PreviewRequest& req) const;
    std::string source_frame_key(const PreviewRequest& req,const std::string& input_path) const;

    std::string ffmpeg_;
    std::string ffprobe_;
    std::string ffplay_;
    // FFmpeg 7+ uses generic file-valued options; older versions use the
    // legacy script option. Remember a capability rejection, not a version.
    mutable std::atomic<bool> legacy_graph_file_option_{false};
    mutable GpuBackendInfo gpu_;
    std::atomic<bool> gpu_enabled_flag_{true};
    mutable FrameCache cache_;
    mutable FrameCache source_cache_{64};
    FrameCache thumbnail_cache_{384};
    mutable std::mutex cache_mutex_;

    std::thread worker_;
    std::thread thumbnail_worker_;
    std::mutex thumbnail_mutex_;
    std::condition_variable thumbnail_cv_;
    struct ThumbnailRequest { std::string path,key; double time=0.0; int width=96,height=54;VectorScene vector_source; };
    std::deque<ThumbnailRequest> thumbnail_queue_;
    std::unordered_set<std::string> thumbnail_pending_;
    std::atomic<std::uint64_t> thumbnail_revision_{0};
    struct MeterSample { double time=0.0,left=0.0,right=0.0; };
    struct AudioEnvelope { std::vector<MeterSample> samples; bool failed=false; };
    std::thread meter_worker_;
    std::thread preparation_worker_;
    mutable std::mutex preparation_mutex_;
    std::condition_variable preparation_cv_;
    std::deque<MediaItem> preparation_queue_;
    std::unordered_set<std::string> preparation_pending_;
    std::unordered_map<std::string,MediaPreparationProgress> preparation_states_;
    std::mutex meter_mutex_;
    std::condition_variable meter_cv_;
    std::deque<std::string> meter_queue_;
    std::unordered_set<std::string> meter_pending_;
    std::unordered_map<std::string,AudioEnvelope> meter_cache_;
    std::atomic<bool> stop_{false};
    PlaybackController playback_controller_;

    // Decoder sessions are owned exclusively by the preview worker.
    DecoderPool decoder_pool_;
    std::unordered_map<int, std::unique_ptr<DecoderPool>> layer_decoders_;
    std::atomic<std::uint64_t> stream_start_count_{0};
    mutable std::atomic<std::uint64_t> source_decode_count_{0};

    struct AudioPlayer {
        DetachedProcess process;
        std::string signature;
    };
    AudioPlayer audio_mix_player_;
    std::string audio_mix_signature_;
    double audio_mix_anchor_ = -1.0;
    double audio_mix_horizon_ = -1.0;
    DetachedProcess audio_recorder_;
    std::string audio_recording_raw_path_;
    int audio_recording_sample_rate_=48000;
    int audio_recording_channels_=1;
};

} // namespace koralo
