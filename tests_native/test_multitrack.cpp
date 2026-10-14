#include "core/edit_controller.hpp"
#include "platform/app_paths.hpp"
#include "media/media_engine.hpp"
#include "ui/localization.hpp"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace koralo;
static void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static Frame preview(MediaEngine& engine,PreviewRequest request){
    static std::uint64_t token=9000;request.token=++token;engine.request_preview(request);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    while(std::chrono::steady_clock::now()<deadline){
        Frame frame;std::string error;std::uint64_t got=0;
        if(engine.poll_preview(got,frame,error)&&got==token){if(!error.empty())throw std::runtime_error(error);check(!frame.rgb.empty(),"preview must contain all requested layers");return frame;}
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    throw std::runtime_error("preview timed out");
}
int main(){try{
    Localization locale({path_to_utf8(std::filesystem::path(__FILE__).parent_path().parent_path()/"locales")});
    check(locale.set_language("ru-RU"),"Russian language pack loads");
    for(const auto* def:available_effect_definitions()){
        check(locale.text("catalog."+def->name,def->name)!=def->name,"video effect name translated");
        for(const auto& param:def->parameters)check(locale.text("catalog."+param.name,param.name)!=param.name,"video parameter translated");
    }
    for(const auto* def:available_audio_effect_definitions()){
        check(locale.text("catalog."+def->name,def->name)!=def->name,"audio effect name translated");
        for(const auto& param:def->parameters)check(locale.text("catalog."+param.name,param.name)!=param.name,"audio parameter translated");
    }
    check(search_casefold("ЯРКОСТЬ ЁЖ RGB")=="яркость ёж rgb","case-insensitive Russian effect search");
    std::string query="Свечение";erase_last_utf8(query);check(query=="Свечени","backspace removes whole Russian character");
    Project project;MediaItem media;media.path="sample.mp4";media.duration=8;media.has_video=media.has_audio=true;media.fps=30;
    const int mid=project.add_media(media).id;
    UndoStack undo;EditController edit;TimelineGeometry geometry;geometry.x=geometry.y=0;geometry.height=600;
    edit.begin_bin_drag(mid,0,0);
    check(edit.commit_drag(project,undo,geometry,geometry.header_w+90,geometry.ruler_h+20,0),"drop on empty timeline creates tracks");
    check(project.tracks.size()==2&&project.clips.size()==2,"AV drop creates video and audio");
    check(undo.undo(project)&&project.tracks.empty()&&project.clips.empty(),"one undo removes automatic tracks and clips");
    check(undo.redo(project)&&project.tracks.size()==2,"redo restores drop");
    const int first=project.clips.front().id;
    check(project.detach_audio(first)&&project.clips.size()==2,"detach linked AV without duplicate audio");
    check(!project.detach_audio(first)&&project.clips.size()==2,"repeated detach does not double sound");
    const int next=project.place_media(mid,1,0,true);
    check(project.tracks.size()==4,"overlapping import creates another AV pair");
    check(project.clip_by_id(next)->group_id>0,"pair linked initially");
    project.track_by_id(project.clip_by_id(next)->track_id)->locked=true;
    check(!project.detach_audio(next),"locked video cannot be detached");
    project.track_by_id(project.clip_by_id(next)->track_id)->locked=false;
    Project extract;const int em=extract.add_media(media).id;const int vt=extract.add_track(TrackKind::Video).id;
    const int video=extract.add_clip(em,vt,2,1.25,3).id;
    extract.clip_by_id(video)->rate=1.5;extract.clip_by_id(video)->fade_in=.3;extract.clip_by_id(video)->gain=.7;
    undo.clear();undo.begin(extract);check(extract.detach_audio(video),"extract missing audio");check(undo.commit(extract),"extract undo transaction");
    check(extract.clips.size()==2&&extract.tracks.size()==2,"audio track created");
    const Clip sound=extract.clips.back();
    check(sound.start==2&&sound.source_offset()==1.25&&sound.duration==3&&sound.rate==1.5&&sound.fade_in==.3&&sound.gain==.7,"extracted audio preserves timing speed fades and gain");
    check(undo.undo(extract)&&extract.clips.size()==1&&extract.tracks.size()==1,"undo extraction restores state");
    // 8 active layers, every layer changes the final expected brightness.
    Project layered;layered.auto_crossfade=false;
    for(int i=0;i<8;++i){MediaItem m=media;m.has_audio=false;const int id=layered.add_media(m).id;const int t=layered.add_track(TrackKind::Video).id;layered.add_clip(id,t,0,0,4);}
    auto layers=build_preview_layers(layered,1,false);check(layers.size()==8,"planner retains more than three layers");
    layered.tracks[3].solo=true;check(build_preview_layers(layered,1,false).size()==1,"solo applies to all layers");layered.tracks[3].solo=false;
    layered.tracks[3].mute=true;check(build_preview_layers(layered,1,false).size()==7,"mute applies to all layers");
    check(build_preview_layers(layered,9,false).empty(),"timeline gaps do not show bin media");
    MediaEngine engine;engine.set_gpu_enabled(false);
    PreviewRequest scene;scene.width=96;scene.height=54;
    for(int i=0;i<8;++i){PreviewRequest layer;layer.stream_id=i+1;layer.opacity=.25;auto fx=make_effect_instance("koralo.brightness_contrast",i+1);set_effect_numeric(fx,"brightness",1.0);layer.effects={fx};scene.layers.push_back(layer);}
    Frame frame=preview(engine,scene);const int expected=int(std::round(255*(1-std::pow(1-64.0/255,8))));
    check(std::abs(int(frame.rgb[0])-expected)<=3,"eight transparent layers compose correctly");
    scene.layers[2].opacity=0;Frame changed=preview(engine,scene);check(changed.rgb[0]<frame.rgb[0],"lower layer edit invalidates composite cache");
    PreviewRequest title;title.stream_id=20;title.has_text_overlay=true;title.text_overlay.enabled=true;title.text_overlay.text="Test";title.text_overlay.font_size=40;
    scene.layers={title};Frame text=preview(engine,scene);int lit=0;for(auto byte:text.rgb)lit+=byte>0;check(lit>0&&lit<int(text.rgb.size()),"text-only layer retains transparent surroundings");
    if(engine.ready()){
        TemporaryFile video_file(temporary_directory(),".mkv");check(video_file.valid(),"reserve video fixture");
        const auto path=path_to_utf8(video_file.path());
        auto process=run_process(engine.ffmpeg_path(),{"-hide_banner","-loglevel","error","-y","-f","lavfi","-i","color=white:s=96x54:r=24:d=3","-c:v","ffv1","-threads","1",path});
        check(process.exit_code==0,"generate playback fixture");
        scene.layers.clear();scene.continuous=true;scene.playback=true;scene.playback_epoch=1;
        for(int i=0;i<6;++i){PreviewRequest layer;layer.path=path;layer.stream_id=i+1;layer.opacity=.25;layer.continuous=true;layer.requested_fps=24;layer.allow_prepared_cache=false;scene.layers.push_back(layer);}
        const auto starts=engine.preview_stream_start_count();
        for(int n=0;n<12;++n){for(auto& layer:scene.layers)layer.time=n/24.0;preview(engine,scene);}
        if(engine.preview_stream_start_count()-starts!=6)std::cerr<<"multitrack starts="<<(engine.preview_stream_start_count()-starts)<<" expected=6\n";check(engine.preview_stream_start_count()-starts==6,"six streams remain open across twelve frames of same-file layers");
        scene.continuous=false;for(auto& layer:scene.layers){layer.continuous=false;layer.time=.25;}preview(engine,scene);
        scene.continuous=true;for(auto& layer:scene.layers){layer.continuous=true;layer.time=.5;}preview(engine,scene);
        check(engine.preview_stream_start_count()-starts==12,"pause releases decoders and resume restarts them");
        const auto opaque_starts=engine.preview_stream_start_count();
        scene.layers.back().stream_id=99;scene.layers.back().opacity=1.0;
        for(auto& layer:scene.layers)layer.time=.6;
        preview(engine,scene);
        check(engine.preview_stream_start_count()-opaque_starts==1,"opaque top video skips hidden lower decoders");
        std::cout<<"Six-layer playback: 12 frames, 6 decoder starts; pause/resume passed\n";
    }else std::cout<<"SKIP playback fixture: FFmpeg unavailable\n";
    std::cout<<"Multitrack regression tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
