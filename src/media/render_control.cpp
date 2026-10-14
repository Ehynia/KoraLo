#include "render_control.hpp"
#include "../platform/process.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__)
#include <sys/resource.h>
#include <sys/sysinfo.h>
#else
#include <ctime>
#endif

namespace koralo {
namespace {
std::string escaped(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (unsigned char c : value) {
        if (c=='"') result+="\\\"";
        else if (c=='\\') result+="\\\\";
        else if (c=='\n') result+="\\n";
        else if (c=='\r') result+="\\r";
        else if (c=='\t') result+="\\t";
        else if (c<32) result+='?';
        else result+=static_cast<char>(c);
    }
    return result;
}
std::uint64_t process_cpu_ns() {
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    const auto ticks=[](FILETIME t) -> std::uint64_t {
        return (static_cast<std::uint64_t>(t.dwHighDateTime)<<32)|t.dwLowDateTime;
    };
    return (ticks(kernel)+ticks(user))*100u;
#elif defined(__linux__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF,&usage)!=0) return 0;
    const auto ns=[](timeval tv) -> std::uint64_t {
        return static_cast<std::uint64_t>(tv.tv_sec)*1000000000ull+
               static_cast<std::uint64_t>(tv.tv_usec)*1000ull;
    };
    return ns(usage.ru_utime)+ns(usage.ru_stime);
#else
    return static_cast<std::uint64_t>(static_cast<double>(std::clock())*1.e9/CLOCKS_PER_SEC);
#endif
}
std::uint64_t available_ram_bytes() {
#ifdef _WIN32
    MEMORYSTATUSEX status{}; status.dwLength=sizeof(status);
    return GlobalMemoryStatusEx(&status) ? static_cast<std::uint64_t>(status.ullAvailPhys) : 0;
#elif defined(__linux__)
    struct sysinfo info{};
    if (sysinfo(&info)!=0) return 0;
    return static_cast<std::uint64_t>(info.freeram+info.bufferram)*info.mem_unit;
#else
    return 0;
#endif
}
std::string probe_value(const std::string& values, const std::string& key) {
    const std::string prefix=key+"=";
    std::istringstream input(values);
    std::string line;
    while (std::getline(input,line)) {
        if (line.rfind(prefix,0)==0) return line.substr(prefix.size());
    }
    return {};
}
} // namespace

RenderControlSnapshot render_control_snapshot(const RenderControlLive& live) {
    return {live.workers.load(), live.worker_cap.load(),live.process_cpu_percent.load(),
            live.available_memory_mb.load(),live.state.load()};
}

RenderControlSession::RenderControlSession(const std::filesystem::path& output,
                                         RenderControlLive* live)
    : start_(std::chrono::steady_clock::now()), live_(live) {
    log_path_=output;
    log_path_+=std::filesystem::path(".render-control.jsonl");
    log_.open(log_path_,std::ios::out|std::ios::trunc|std::ios::binary);
    if (live_) {
        live_->state=1; live_->workers=0;live_->worker_cap=0;
        live_->process_cpu_percent=0;live_->available_memory_mb=0;
    }
    record("render_start",path_to_utf8(output));
}
RenderControlSession::~RenderControlSession()=default;
void RenderControlSession::record(const std::string& event,const std::string& message) {
    std::lock_guard lock(mutex_);
    if (!log_) return;
    const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now()-start_).count();
    log_<<"{\"elapsed_ms\":"<<ms<<",\"event\":\""<<escaped(event)
        <<"\",\"message\":\""<<escaped(message)<<"\"}\n";
    log_.flush();
}
void RenderControlSession::update_workers(unsigned active,unsigned cap,int cpu_percent,
                                          std::uint64_t available_mb) {
    if (live_) {
        live_->workers=active; live_->worker_cap=cap;
        live_->process_cpu_percent=cpu_percent;
        live_->available_memory_mb=available_mb;
    }
    record("resource_sample","workers="+std::to_string(active)+"/"+std::to_string(cap)+
           " cpu_process_percent="+std::to_string(cpu_percent)+
           " available_ram_mb="+std::to_string(available_mb));
}
void RenderControlSession::stage(int value,const std::string& message) {
    if (live_) live_->state=value;
    record("stage",message);
}
void RenderControlSession::finish(bool success,const std::string& detail) {
    if (live_) {live_->state=success?3:4;live_->workers=0;}
    record(success?"render_success":"render_failed",detail);
}

AdaptiveRenderWorkers::AdaptiveRenderWorkers(unsigned maximum, std::size_t rgba_frame_bytes,
                                             bool fixed_override)
    : maximum_(std::max(1u,maximum)),active_(fixed_override?maximum_:std::min(2u,maximum_)),
      reserved_bytes_(rgba_frame_bytes>std::numeric_limits<std::uint64_t>::max()/6 ?
                      std::numeric_limits<std::uint64_t>::max() : rgba_frame_bytes*6ull),
      fixed_(fixed_override),last_(std::chrono::steady_clock::now()),
      last_process_cpu_ns_(process_cpu_ns()) {}

bool AdaptiveRenderWorkers::sample() {
    const auto now=std::chrono::steady_clock::now();
    const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(now-last_).count();
    if (ns < 1000000000ll) return false;
    const auto current=process_cpu_ns();
    if (current >= last_process_cpu_ns_ && last_process_cpu_ns_ && ns>0) {
        const double per_core=double(current-last_process_cpu_ns_)/double(ns);
        cpu_percent_=std::clamp(int(std::lround(per_core*100.0)),0,10000);
    }
    last_process_cpu_ns_=current;
    last_=now;
    const auto bytes=available_ram_bytes();
    available_mb_=bytes/(1024ull*1024ull);
    if (fixed_) return true;
    // Protect available physical memory; keep a reserve for OS, decoder and GUI.
    if (bytes && bytes < 512ull*1024ull*1024ull+reserved_bytes_*active_) {
        active_=std::max(1u,active_/2);
        low_cpu_samples_=0;
    } else if (cpu_percent_<int(std::max(1u,std::thread::hardware_concurrency())*70u) &&
               active_<maximum_) {
        // Hysteresis prevents oscillating when I/O stalls briefly.
        if (++low_cpu_samples_>=2) {++active_;low_cpu_samples_=0;}
    } else {
        low_cpu_samples_=0;
    }
    return true;
}

bool verify_rendered_video(const std::string& ffprobe, const std::string& ffmpeg,
                           const std::filesystem::path& candidate,
                           int expected_width,int expected_height,double expected_duration,
                           const std::atomic<bool>* cancel,std::string* error,
                           bool expect_audio,RenderControlSession* session) {
    const auto fail=[&](const std::string& detail){
        if (error) *error="Export verification failed: "+detail;
        if (session) session->record("verify_failure",detail);
        return false;
    };
    if (session) session->stage(2,"Checking output streams and decoded frames");
    if (cancel && cancel->load()) return fail("cancelled");
    std::error_code ec;
    if (!std::filesystem::is_regular_file(candidate,ec) ||
        std::filesystem::file_size(candidate,ec)<256 || ec)
        return fail("output missing or empty");
    if (ffprobe.empty() || ffmpeg.empty()) return fail("ffprobe or ffmpeg unavailable");
    ProcessOptions opt; opt.timeout_ms=120000;opt.cancel=cancel;opt.max_output_bytes=65536;
    const auto probed=run_process(ffprobe, {"-v","error","-select_streams","v:0",
        "-show_entries","stream=width,height:format=duration",
        "-of","default=noprint_wrappers=1",path_to_utf8(candidate)},opt);
    if (!probed.started || probed.exit_code!=0 || probed.cancelled)
        return fail("ffprobe rejected the container: "+probed.output);
    int width=0,height=0;double duration=0;
    try {
        const auto ws=probe_value(probed.output,"width"),hs=probe_value(probed.output,"height"),
                   ds=probe_value(probed.output,"duration");
        if (ws.empty()||hs.empty()||ds.empty()) return fail("missing video stream or duration");
        width=std::stoi(ws);height=std::stoi(hs);duration=std::stod(ds);
    } catch (...) { return fail("unreadable dimensions or duration"); }
    if (width!=expected_width || height!=expected_height)
        return fail("resolution mismatch: "+std::to_string(width)+"x"+std::to_string(height));
    const double tolerance=std::max(0.75,expected_duration*0.03);
    if (!std::isfinite(duration)||duration<=0||
        (expected_duration>0&&std::abs(duration-expected_duration)>tolerance))
        return fail("duration mismatch: "+std::to_string(duration)+"s expected "+
                    std::to_string(expected_duration)+"s");
    if (expect_audio) {
        const auto audio=run_process(ffprobe,{"-v","error","-select_streams","a:0",
            "-show_entries","stream=codec_name","-of","default=noprint_wrappers=1",path_to_utf8(candidate)},opt);
        if (!audio.started || audio.exit_code!=0 ||
            probe_value(audio.output,"codec_name").empty())
            return fail("expected audio stream is missing");
    }
    if (cancel && cancel->load()) return fail("cancelled");
    // A complete decode detects truncated tails that ffprobe can still report
    // as valid. No output file is touched by the validation command.
    ProcessOptions decode_opts;decode_opts.timeout_ms=0;decode_opts.cancel=cancel;
    decode_opts.max_output_bytes=65536;
    const auto decoded=run_process(ffmpeg,{"-nostdin","-hide_banner","-v","error",
        "-xerror","-err_detect","explode","-i",path_to_utf8(candidate),
        "-map","0:v:0","-map","0:a:0?","-f","null","-"},decode_opts);
    if (!decoded.started||decoded.exit_code!=0||decoded.cancelled)
        return fail("full decode failed: "+decoded.output);
    if (session) session->record("verify_ok",std::to_string(width)+"x"+
        std::to_string(height)+" duration="+std::to_string(duration)+"s full_decode=ok");
    return true;
}
} // namespace koralo
