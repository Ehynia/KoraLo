#include "audio_mix_graph.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>

namespace koralo {
namespace {
std::string num(double x) {
    std::ostringstream s;
    s << std::fixed << std::setprecision(6) << x;
    return s.str();
}
// Two levels of FFmpeg escaping (filter graph + amovie options). In particular
// a Windows drive letter must survive both layers. Neither shell nor cmd.exe
// ever interprets this string: it is passed as one process argument.
std::string movie_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    std::string out;
    out.reserve(path.size() * 2);
    const bool quote = path.find('\'') == std::string::npos;
    if (quote) out.push_back('\'');
    for (char c: path) {
        if (c == ':') out += quote ? "\\:" : "\\\\:";
        else if (c == '\'' && !quote) out += "\\\\\\'";
        else if (!quote && (c == ' ' || c == ',' || c == ';' || c == '[' || c == ']')) {
            out.push_back('\\'); out.push_back(c);
        } else out.push_back(c);
    }
    if (quote) out.push_back('\'');
    return out;
}
std::string tempo(double rate) {
    rate=std::clamp(rate,0.01,100.0);
    std::ostringstream f;
    while(rate>2.0){f<<"atempo=2,";rate/=2;}
    while(rate<0.5){f<<"atempo=0.5,";rate*=2;}
    return f.str()+"atempo="+num(rate);
}
} // namespace

AudioMixPlan plan_audio_mix(const Project& project, double anchor,
                           double horizon_seconds, std::size_t max_sources) {
    AudioMixPlan plan;
    if (!std::isfinite(anchor) || anchor < 0.0 ||
        !std::isfinite(horizon_seconds) || horizon_seconds <= 0.0 ||
        max_sources == 0) {plan.complete=false; return plan;}
    plan.anchor=anchor;
    plan.horizon=std::min(project.duration(),anchor+std::clamp(horizon_seconds,1.0,300.0));
    const bool any_solo=std::any_of(project.tracks.begin(),project.tracks.end(),
        [](const Track& t){return t.kind==TrackKind::Audio&&t.solo;});
    struct Candidate {const Clip* clip;const Track* track;const MediaItem* media;double at;};
    std::vector<Candidate> candidates;
    for (const auto& clip:project.clips) {
        const Track* track=project.track_by_id(clip.track_id);
        const MediaItem* media=project.media_by_id(clip.media_id());
        if (!track||!media||track->kind!=TrackKind::Audio||!media->has_audio||
            track->mute||clip.muted||(any_solo&&!track->solo)||
            media->path.empty()||clip.end()<=anchor||clip.start>=plan.horizon) continue;
        if (!std::isfinite(clip.start)||!std::isfinite(clip.duration)||
            !std::isfinite(clip.rate)||clip.rate<=0.0) continue;
        candidates.push_back({&clip,track,media,std::max(anchor,clip.start)});
    }
    if(candidates.size()>max_sources){plan.complete=false;return plan;}
    while(plan.segments.size()<max_sources){
        auto next=std::min_element(candidates.begin(),candidates.end(),
            [](const Candidate& x,const Candidate& y){return x.at<y.at;});
        if(next==candidates.end()||!std::isfinite(next->at)||next->at>=plan.horizon-1e-7)break;
        auto& candidate=*next;
        const Clip& clip=*candidate.clip;
        const Track* track=candidate.track;
        const MediaItem* media=candidate.media;
        const double at=candidate.at;
        const auto window=audio_preview_window(clip,*media,at);
        if(!window){candidate.at=std::numeric_limits<double>::infinity();continue;}
        double timeline_length=std::min(window->timeline_length,
            std::min(plan.horizon,clip.end())-at);
        if(clip.loop&&media->duration>1e-7&&std::isfinite(media->duration))
            timeline_length=std::min(timeline_length,(media->duration-window->source_offset)/clip.rate);
        if(timeline_length<=1e-7 || !std::isfinite(timeline_length)){
            candidate.at=std::numeric_limits<double>::infinity();continue;
        }
            AudioMixSegment segment{&clip,track,media,window->source_offset,
                timeline_length*clip.rate,timeline_length,at-anchor,at-clip.start,1.0,{}};
            const auto duck_fx=std::find_if(track->audio_effects.begin(),track->audio_effects.end(),
                [](const EffectInstance& fx){return fx.enabled && fx.definition_id=="koralo.audio.auto_duck";});
            if (duck_fx!=track->audio_effects.end()) {
                segment.duck_gain=std::pow(10.0,std::clamp(effect_numeric(*duck_fx,"reduction_db",0,-12.0),-36.0,0.0)/20.0);
                const double attack=std::clamp(effect_numeric(*duck_fx,"attack_ms",0,120.0),0.0,2000.0)/1000.0;
                const double release=std::clamp(effect_numeric(*duck_fx,"release_ms",0,350.0),0.0,4000.0)/1000.0;
                const auto it=std::find_if(project.tracks.begin(),project.tracks.end(),
                    [track](const Track& t){return t.id==track->id;});
                const auto lower_index=static_cast<std::size_t>(std::distance(project.tracks.begin(),it));
                for(const auto& above:project.clips){
                    const Track* other=project.track_by_id(above.track_id);
                    const MediaItem* source=project.media_by_id(above.media_id());
                    if(!other||!source||other->kind!=TrackKind::Audio||!source->has_audio||
                       other->mute||above.muted||(any_solo&&!other->solo)) continue;
                    const auto above_track=std::find_if(project.tracks.begin(),project.tracks.end(),
                        [other](const Track& t){return t.id==other->id;});
                    if(above_track==project.tracks.end() ||
                       static_cast<std::size_t>(std::distance(project.tracks.begin(),above_track))>=lower_index)continue;
                    double end=above.end();
                    if(!above.loop&&source->duration>0&&above.rate>0)
                        end=std::min(end,above.start+(source->duration-above.source_offset())/above.rate);
                    const double begin=std::max(0.0,above.start-attack-anchor);
                    const double finish=std::min(plan.horizon-anchor,end+release-anchor);
                    if(finish>begin)segment.duck_windows.emplace_back(begin,finish);
                }
            }
        plan.segments.push_back(std::move(segment));
        candidate.at+=timeline_length;
        if(candidate.at>=std::min(plan.horizon,clip.end())-1e-7)
            candidate.at=std::numeric_limits<double>::infinity();
    }
    // A short loop may need hundreds of virtual segments. Rather than drop
    // another active track after the limit, shorten the session horizon and
    // build a new graph when playback reaches it.
    double earliest=std::numeric_limits<double>::infinity();
    for(const auto& c:candidates)earliest=std::min(earliest,c.at);
    if(std::isfinite(earliest)&&earliest<plan.horizon-1e-7){
        if(earliest<=anchor+1e-7){plan.complete=false;return plan;}
        plan.horizon=earliest;
        for(auto& segment:plan.segments){
            const double available=std::max(0.0,earliest-anchor-segment.delay_seconds);
            segment.timeline_seconds=std::min(segment.timeline_seconds,available);
            segment.source_seconds=segment.timeline_seconds*segment.clip->rate;
        }
        plan.segments.erase(std::remove_if(plan.segments.begin(),plan.segments.end(),
            [](const auto& segment){return segment.timeline_seconds<1e-7;}),plan.segments.end());
    }
    std::sort(plan.segments.begin(),plan.segments.end(),[](const auto& a,const auto& b){
        if (a.delay_seconds==b.delay_seconds) return a.clip->id<b.clip->id;
        return a.delay_seconds<b.delay_seconds;
    });
    return plan;
}

std::optional<std::string> build_audio_mix_lavfi(
    const AudioMixPlan& plan,const AudioEffectAppender& append_effects) {
    if (!plan.complete || plan.segments.empty() || !append_effects) return std::nullopt;
    std::ostringstream fg;
    for (std::size_t i=0;i<plan.segments.size();++i) {
        const auto& s=plan.segments[i];
        if(i)fg<<';';
        const double rate=std::clamp(s.clip->rate,0.01,100.0);
        const double volume=std::max(0.0,s.clip->gain*s.track->gain);
        const double pan=std::clamp(s.track->pan,-1.0,1.0);
        const long delay_ms=static_cast<long>(std::llround(s.delay_seconds*1000.0));
        fg<<"amovie=filename="<<movie_path(s.media->path)
          <<":seek_point="<<num(s.source_offset)
          <<",atrim=duration="<<num(s.source_seconds)
          <<','<<tempo(rate)<<",asetpts=PTS-STARTPTS"
          <<",atrim=duration="<<num(s.timeline_seconds)
          <<",volume='"<<num(volume);
        if(s.clip->fade_in>1e-7)
            fg<<"*max(0,min(1,(t+"<<num(s.event_time)<<")/"<<num(s.clip->fade_in)<<"))";
        if(s.clip->fade_out>1e-7)
            fg<<"*max(0,min(1,("<<num(s.clip->duration)<<"-t-"<<num(s.event_time)
              <<")/"<<num(s.clip->fade_out)<<"))";
        fg<<"':eval=frame";
        if(std::abs(pan)>0.0001)
            fg<<",aformat=channel_layouts=stereo,stereotools=balance_out="<<num(pan);
        append_effects(fg,s.clip->audio_effects);
        append_effects(fg,s.track->audio_effects);
        fg<<",aformat=sample_rates=48000:channel_layouts=stereo,adelay="
          <<delay_ms<<'|'<<delay_ms;
        if (s.duck_gain<0.99999 && !s.duck_windows.empty()) {
            fg<<",volume='if(gt(";
            for(std::size_t k=0;k<s.duck_windows.size();++k){
                if(k)fg<<'+';
                fg<<"between(t,"<<num(s.duck_windows[k].first)<<','<<num(s.duck_windows[k].second)<<')';
            }
            fg<<",0),"<<num(s.duck_gain)<<",1)':eval=frame";
        }
        fg<<"[a"<<i<<']';
    }
    fg<<';';
    for(std::size_t i=0;i<plan.segments.size();++i)fg<<"[a"<<i<<']';
    if(plan.segments.size()==1)fg<<"anull";
    else fg<<"amix=inputs="<<plan.segments.size()<<":duration=longest:normalize=0";
    fg<<",atrim=duration="<<num(plan.horizon-plan.anchor)<<",aresample=48000";
    return fg.str();
}
} // namespace koralo
