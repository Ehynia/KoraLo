#include "media/audio_mix_graph.hpp"
#include "platform/process.hpp"
#include "platform/app_paths.hpp"
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace koralo;
namespace fs=std::filesystem;
static void check(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
static bool near(double a,double b){return std::abs(a-b)<1e-5;}
int main(){
    Project project=Project::make_default();
    project.settings.width=160;project.settings.height=90;
    int a=0;
    for(const auto& t:project.tracks)if(t.kind==TrackKind::Audio){a=t.id;break;}
    const int b=project.add_track(TrackKind::Audio,"Background").id;
    MediaItem first;first.has_audio=true;first.duration=5;first.path="/tmp/audio:mix primary.wav";
    MediaItem second=first;second.path="/tmp/audio mix,secondary.wav";
    const int first_id=project.add_media(first).id;
    const int second_id=project.add_media(second).id;
    Clip& c1=project.add_clip(first_id,b,0,0,2.0);c1.fade_in=0.2;c1.fade_out=0.3;
    const int c1_id=c1.id;
    Clip& c2=project.add_clip(second_id,a,1,0,2.0);c2.gain=0.5;
    const int c2_id=c2.id;
    const auto plan=plan_audio_mix(project,0.0,4.0);
    check(plan.complete&&plan.segments.size()==2,"two active/forward timeline sources planned");
    check(near(plan.segments[0].delay_seconds,0)&&near(plan.segments[1].delay_seconds,1),"future source correctly scheduled");
    const auto graph=build_audio_mix_lavfi(plan,[](auto&,const auto&){});
    check(graph.has_value()&&graph->find("amix=inputs=2")!=std::string::npos,"one synchronized mix output");
    check(graph->find("adelay=1000|1000")!=std::string::npos,"source aligned by sample delay");
    check(graph->find("fade")==std::string::npos,"fade is an explicit expression, not a second process");
    project.track_by_id(a)->mute=true;
    check(plan_audio_mix(project,0,4).segments.size()==1,"muted track not mixed");
    project.track_by_id(a)->mute=false;
    project.track_by_id(b)->solo=true;
    check(plan_audio_mix(project,0,4).segments.size()==1,"solo excludes other track");
    project.track_by_id(b)->solo=false;
    project.track_by_id(b)->audio_effects.push_back(
        make_audio_effect_instance("koralo.audio.auto_duck",project.next_id()));
    const auto ducked=plan_audio_mix(project,0,4);
    const auto ducked_clip=std::find_if(ducked.segments.begin(),ducked.segments.end(),
        [c1_id](const auto& segment){return segment.clip->id==c1_id;});
    check(ducked_clip!=ducked.segments.end()&&ducked_clip->duck_gain<0.9&&
          !ducked_clip->duck_windows.empty(),"auto duck active only for audible track above");
    project.track_by_id(b)->audio_effects.clear();
    project.clip_by_id(c2_id)->muted=true;
    check(plan_audio_mix(project,0,4).segments.size()==1,"muted event excluded");
    project.clip_by_id(c2_id)->muted=false;
    auto later=plan_audio_mix(project,1.5,1.0);
    check(later.complete&&later.segments.size()==2,"seek rebuilds graph from playhead");
    check(near(later.segments[0].source_offset,1.5)||near(later.segments[1].source_offset,1.5),"seek skips consumed audio");
    project.clip_by_id(c1_id)->loop=true;
    project.media_by_id(first_id)->duration=0.5;
    auto looped=plan_audio_mix(project,0,2.0);
    check(looped.segments.size()>=5,"looped source splits at physical media boundary");
    check(!plan_audio_mix(project,0,90,1).complete,"source bound prevents unbounded memory use");
    project.media_by_id(first_id)->duration=0.25;
    project.clip_by_id(c1_id)->duration=80.0;
    auto bounded=plan_audio_mix(project,0.0,90.0,48);
    check(bounded.complete && bounded.horizon>0.0 && bounded.horizon<90.0,
          "short loops shorten scheduling window without losing sound");
    const bool has_lower=std::any_of(bounded.segments.begin(),bounded.segments.end(),
        [c1_id](const auto& seg){return seg.clip->id==c1_id;});
    const bool has_upper=std::any_of(bounded.segments.begin(),bounded.segments.end(),
        [c2_id](const auto& seg){return seg.clip->id==c2_id;});
    check(has_lower&&has_upper,"bounded loop never starves another audio track");

    const auto ffmpeg = find_executable_on_path("ffmpeg");
    const auto ffprobe = find_executable_on_path("ffprobe");
    if(ffmpeg.empty()||ffprobe.empty()){std::cout<<"SKIP: no FFmpeg\n";return 77;}
    const fs::path temp=fs::temp_directory_path()/"koralo audio mix regression";
    fs::create_directories(temp);
    const auto first_file=path_to_utf8(temp/"audio:mix primary.wav");
    const auto second_file=path_to_utf8(temp/"audio mix,secondary.wav");
    const auto output=path_to_utf8(temp/"mixed audio.wav");
    const auto generate=[&](const std::string& path,int frequency){
        const auto result=run_process(ffmpeg,{"-hide_banner","-loglevel","error","-y","-f","lavfi","-i","sine=frequency="+std::to_string(frequency)+":duration=3:sample_rate=48000","-c:a","pcm_s16le",path});
        check(result.started&&result.exit_code==0,"generate synthesized test tone");
    };
    generate(first_file,440);generate(second_file,880);
    project.media_by_id(first_id)->path=first_file;
    project.media_by_id(first_id)->duration=3;
    project.media_by_id(second_id)->path=second_file;
    project.clip_by_id(c1_id)->loop=false;
    project.clip_by_id(c1_id)->duration=2.0;
    project.track_by_id(b)->audio_effects.push_back(
        make_audio_effect_instance("koralo.audio.auto_duck",project.next_id()));
    const auto duck_lavfi=build_audio_mix_lavfi(plan_audio_mix(project,0.0,3.0),[](auto&,const auto&){});
    check(duck_lavfi && duck_lavfi->find("volume='if(gt(")!=std::string::npos,
          "duck envelope included in playback graph");
    const auto duck_result=run_process(ffmpeg,{"-hide_banner","-loglevel","error","-f","lavfi","-i",*duck_lavfi,"-f","null","-"});
    if(duck_result.exit_code!=0)std::cerr<<duck_result.output<<'\n';
    check(duck_result.started&&duck_result.exit_code==0,"real FFmpeg auto duck graph parses");
    project.track_by_id(b)->audio_effects.clear();
    const auto lavfi=build_audio_mix_lavfi(plan_audio_mix(project,0.0,3.0),[](auto&,const auto&){});
    check(lavfi.has_value(),"build real two-input lavfi mix");
    const auto rendered=run_process(ffmpeg,{
        "-hide_banner","-loglevel","error","-y","-f","lavfi","-i",*lavfi,
        "-c:a","pcm_s16le",output});
    if(rendered.exit_code!=0)std::cerr<<rendered.output<<'\n';
    check(rendered.started&&rendered.exit_code==0,"real FFmpeg lavfi mix export");
    const auto probe=run_process(ffprobe,{"-v","error","-show_entries","format=duration",
                                       "-of","default=noprint_wrappers=1:nokey=1",output});
    check(probe.started&&probe.exit_code==0,"probe mixed PCM");
    check(std::stod(probe.output)>2.9,"mixed PCM has correct timeline duration");
    std::error_code ec;fs::remove_all(temp,ec);
    std::cout<<"KoraLo audio mix planner + two-source FFmpeg graph PASS\n";
}
