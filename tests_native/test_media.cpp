#include "media/media_engine.hpp"
#include "platform/process.hpp"
#include "platform/app_paths.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace koralo;
namespace fs = std::filesystem;

static void req(bool ok,const char*m){if(!ok)throw std::runtime_error(m);}

int main(){
    MediaEngine e;
    if(!e.ready()){std::cout<<"SKIP: ffmpeg unavailable\n";return 77;}
    auto ffprobe=MediaEngine::locate_binary("ffprobe");
    if(ffprobe.empty()){std::cout<<"SKIP: ffprobe unavailable\n";return 0;}

    // Regression coverage for Windows user profiles containing non-ASCII characters
    // and for paths containing spaces. KoraLo's path strings are UTF-8.
    const std::string unicode_dir_name="koralo utf8 \xD1\x82\xD0\xB5\xD1\x81\xD1\x82";
    // Concurrent test runs must never share inputs or remove each other's outputs.
    TemporaryFile reservation(temporary_directory(),".media-tests");
    req(reservation.valid(),"reserve isolated media test directory");
    const fs::path test_root=reservation.release();
    std::error_code reserve_error;fs::remove(test_root,reserve_error);
    req(!reserve_error,"release media test reservation");
    struct TestCleanup{fs::path root;~TestCleanup(){std::error_code error;fs::remove_all(root,error);}} cleanup{test_root};
    const fs::path dir=test_root/path_from_utf8(unicode_dir_name);
    std::error_code ec;
    fs::create_directories(dir,ec);
    req(!ec,"create unicode temp directory");
    const fs::path src_path=dir/"koralo native media test.mp4";
    const fs::path out_path=dir/"koralo native render test.mp4";
    const fs::path fx_out_path=dir/"koralo controls render test.mp4";
    const fs::path motion_out_path=dir/"koralo motion render test.mp4";
    const fs::path preset_out_path=dir/"koralo preset render test.mp4";
    const fs::path long_src_path=dir/"koralo long cache test.mp4";
    const fs::path still_path=dir/"koralo still image.bmp";
    const fs::path odd_still_path=dir/"koralo odd still image.jpg";
    const fs::path chroma_green_path=dir/"koralo chroma green.bmp";
    const fs::path chroma_red_path=dir/"koralo chroma red.bmp";
    const fs::path chroma_render_path=dir/"koralo chroma composite render.mp4";
    const fs::path still_render_path=dir/"koralo still render.mp4";
    const fs::path text_render_path=dir/"koralo generated text render.mp4";
    const fs::path anime_render_path=dir/"koralo anime restore filters render.mp4";
    const fs::path free_fx_render_path=dir/"koralo free editing effects render.mp4";
    const fs::path audio_fx_render_path=dir/"koralo audio effects render.mp4";
    const fs::path raw_record_path=dir/"koralo recording raw.pcm";
    const fs::path wav_record_path=dir/"koralo recording finalized.wav";
    const std::string src=path_to_utf8(src_path);
    const std::string out=path_to_utf8(out_path);
    const std::string fx_out=path_to_utf8(fx_out_path);
    const std::string motion_out=path_to_utf8(motion_out_path);
    const std::string preset_out=path_to_utf8(preset_out_path);
    const std::string long_src=path_to_utf8(long_src_path);
    const std::string still=path_to_utf8(still_path);
    const std::string odd_still=path_to_utf8(odd_still_path);
    const std::string chroma_green=path_to_utf8(chroma_green_path);
    const std::string chroma_red=path_to_utf8(chroma_red_path);
    const std::string chroma_render=path_to_utf8(chroma_render_path);
    const std::string still_render=path_to_utf8(still_render_path);
    const std::string text_render=path_to_utf8(text_render_path);
    const std::string anime_render=path_to_utf8(anime_render_path);
    const std::string free_fx_render=path_to_utf8(free_fx_render_path);
    const std::string audio_fx_render=path_to_utf8(audio_fx_render_path);

    const auto generated=run_process(e.ffmpeg_path(),{
        "-hide_banner","-loglevel","error","-y",
        "-f","lavfi","-i","testsrc2=size=320x180:rate=30",
        "-f","lavfi","-i","sine=frequency=440:sample_rate=48000",
        "-t","1","-c:v","libx264","-preset","ultrafast","-c:a","aac",src
    });
    if(!generated.output.empty() && generated.exit_code!=0) std::cerr<<generated.output;
    req(generated.started&&generated.exit_code==0,"generate sample");

    std::string err;
    {
        std::ofstream raw(raw_record_path,std::ios::binary|std::ios::trunc);for(int i=0;i<4800;++i){const short sample=static_cast<short>((i%100)-50);raw.write(reinterpret_cast<const char*>(&sample),sizeof(sample));}
    }
    double recorded_duration=0.0;err.clear();req(MediaEngine::finalize_pcm16le_recording(path_to_utf8(raw_record_path),path_to_utf8(wav_record_path),48000,1,&recorded_duration,&err),"finalize raw microphone PCM to WAV");req(recorded_duration>0.09&&recorded_duration<0.11,"recorded WAV duration");auto recorded_probe=e.probe(path_to_utf8(wav_record_path),&err);req(recorded_probe.has_value()&&recorded_probe->has_audio&&!recorded_probe->has_video,"finalized recording probes as audio");

    auto m=e.probe(src,&err);
    if(!m && !err.empty()) std::cerr<<err;
    req(m.has_value(),"probe");
    req(m->has_video&&m->has_audio,"streams");
    req(m->width==320&&m->height==180,"dimensions");

    e.request_preview({src,0.2,320,180,42});
    std::uint64_t tok=0;Frame f;
    for(int i=0;i<200;++i){if(e.poll_preview(tok,f,err))break;std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    if(f.rgb.empty() && !err.empty()) std::cerr<<err;
    req(tok==42&&!f.rgb.empty(),"preview frame");

    // A container can hold a larger embedded cover as well as moving video.
    // Probe, paused preview, playback and export must all select real video.
    const auto cover_path=dir/"cover.ppm",covered_path=dir/"movie with cover.mp4";
    {std::ofstream cover(cover_path,std::ios::binary);cover<<"P6\n480 320\n255\n";
     for(int i=0;i<480*320;++i){const char pixel[]{char(255),0,char(255)};cover.write(pixel,3);}}
    const auto attach=run_process(e.ffmpeg_path(),{"-nostdin","-v","error","-y","-i",src,"-i",path_to_utf8(cover_path),
        "-map","0:v:0","-map","0:a:0","-map","1:v:0","-c","copy","-c:v:1","png","-threads:v:1","1",
        "-disposition:v:0","0","-disposition:v:1","attached_pic",path_to_utf8(covered_path)});
    if(attach.exit_code!=0)std::cerr<<attach.output;
    req(attach.started&&attach.exit_code==0,"create embedded-cover fixture");
    const auto covered=e.probe(path_to_utf8(covered_path),&err);
    req(covered&&covered->width==320&&covered->height==180&&covered->fps>29,"probe ignores cover dimensions");
    for(bool continuous:{false,true}){
        PreviewRequest cover_req;cover_req.path=path_to_utf8(covered_path);cover_req.time=.2;
        cover_req.width=320;cover_req.height=180;cover_req.token=continuous?46:45;
        cover_req.continuous=continuous;cover_req.requested_fps=30;cover_req.allow_prepared_cache=false;
        e.request_preview(cover_req);Frame actual;std::uint64_t token=0;err.clear();
        for(int i=0;i<400;++i){if(e.poll_preview(token,actual,err)&&token==cover_req.token)break;std::this_thread::sleep_for(std::chrono::milliseconds(5));}
        if(actual.rgb.empty())std::cerr<<err;
        req(token==cover_req.token&&!actual.rgb.empty(),"embedded-cover movie previews at nonzero seek");
        req(actual.rgb==f.rgb,"preview decodes movie pixels instead of cover");
    }
    Project covered_project=Project::make_default();covered_project.settings.width=320;covered_project.settings.height=180;
    const int covered_id=covered_project.add_media(*covered).id;
    int covered_track=0;for(const auto&t:covered_project.tracks)if(t.kind==TrackKind::Video){covered_track=t.id;break;}
    covered_project.add_clip(covered_id,covered_track,0,.2,.4);err.clear();
    req(e.render_project(covered_project,path_to_utf8(dir/"covered render.mp4"),nullptr,&err),"export embedded-cover movie");

    // Realtime playback regression: consecutive frames must come from one
    // persistent decoder session instead of launching FFmpeg per frame.
    const auto stream_starts_before=e.preview_stream_start_count();
    for(int frame_index=0;frame_index<8;++frame_index){
        PreviewRequest reqp;reqp.path=src;reqp.time=0.05+frame_index/30.0;reqp.width=320;reqp.height=180;reqp.token=100+frame_index;reqp.continuous=true;reqp.requested_fps=30.0;reqp.source_rate=1.0;reqp.allow_prepared_cache=false;
        e.request_preview(reqp);
        Frame streamed;std::uint64_t streamed_token=0;std::string streamed_error;
        for(int i=0;i<250;++i){if(e.poll_preview(streamed_token,streamed,streamed_error)&&streamed_token==reqp.token)break;std::this_thread::sleep_for(std::chrono::milliseconds(4));}
        if(streamed.rgb.empty()&&!streamed_error.empty())std::cerr<<streamed_error;
        req(streamed_token==reqp.token&&!streamed.rgb.empty(),"persistent preview stream frame");
    }
    const auto stream_starts=e.preview_stream_start_count()-stream_starts_before;
    req(stream_starts>=1&&stream_starts<=2,"persistent preview reuses decoder process");

    // FX-heavy playback must stay on the same persistent decoder stream; effects
    // are applied in-process instead of spawning a new FFmpeg process per frame.
    auto fx_preview=make_effect_instance("koralo.brightness_contrast",77);set_effect_numeric(fx_preview,"brightness",0.08);set_effect_numeric(fx_preview,"contrast",0.15);
    const auto fx_stream_before=e.preview_stream_start_count();
    for(int frame_index=0;frame_index<6;++frame_index){
        PreviewRequest reqp;reqp.path=src;reqp.time=0.10+frame_index/30.0;reqp.width=320;reqp.height=180;reqp.token=140+frame_index;reqp.continuous=true;reqp.requested_fps=30.0;reqp.source_rate=1.0;reqp.allow_prepared_cache=false;reqp.effects={fx_preview};reqp.effect_time=frame_index/30.0;
        e.request_preview(reqp);Frame streamed;std::uint64_t streamed_token=0;std::string streamed_error;for(int i=0;i<250;++i){if(e.poll_preview(streamed_token,streamed,streamed_error)&&streamed_token==reqp.token)break;std::this_thread::sleep_for(std::chrono::milliseconds(4));}req(streamed_token==reqp.token&&!streamed.rgb.empty(),"persistent FX preview stream frame");
    }
    const auto fx_stream_starts=e.preview_stream_start_count()-fx_stream_before;req(fx_stream_starts<=2,"FX preview does not respawn decoder every frame");

    // Anime Restore pack: the user-provided restoration pipeline is exposed as
    // independent native filters rather than a Python runtime dependency.
    std::vector<EffectInstance> anime_fx;
    for(const char* id:{"koralo.anime_line_restore","koralo.anime_vector_color","koralo.anime_text_restore","koralo.anime_paper_texture","koralo.anime_detail_sharpen","koralo.anime_deinterlace"})
        anime_fx.push_back(make_effect_instance(id,200+static_cast<int>(anime_fx.size())));
    PreviewRequest anime_req;anime_req.path=src;anime_req.time=0.2;anime_req.width=320;anime_req.height=180;anime_req.token=170;anime_req.allow_prepared_cache=false;anime_req.effects=anime_fx;anime_req.effect_time=0.2;
    e.request_preview(anime_req);Frame anime_frame;std::uint64_t anime_token=0;for(int i=0;i<300;++i){if(e.poll_preview(anime_token,anime_frame,err)&&anime_token==170)break;std::this_thread::sleep_for(std::chrono::milliseconds(4));}
    req(anime_token==170&&!anime_frame.rgb.empty(),"anime restore filter preview");
    std::uint64_t anime_delta=0;for(std::size_t i=0;i<std::min(f.rgb.size(),anime_frame.rgb.size());++i)anime_delta+=static_cast<std::uint64_t>(std::abs(static_cast<int>(f.rgb[i])-static_cast<int>(anime_frame.rgb[i])));req(anime_delta>1000,"anime restore filters change preview pixels");

    // Free editing pack: clean-room native versions of common free-filter
    // concepts must all execute in the in-process preview path.
    std::vector<EffectInstance> free_fx;
    for(const char* id:{"koralo.glow","koralo.hue_shift","koralo.levels","koralo.vignette","koralo.pixelate","koralo.lens_distortion","koralo.rgb_split","koralo.letterbox","koralo.scanlines"})
        free_fx.push_back(make_effect_instance(id,240+static_cast<int>(free_fx.size())));
    set_effect_numeric(free_fx[1],"degrees",35.0);set_effect_numeric(free_fx[2],"gamma",1.15);set_effect_numeric(free_fx[4],"block",6.0);set_effect_numeric(free_fx[5],"k1",0.08);set_effect_numeric(free_fx[6],"horizontal",4.0);
    PreviewRequest free_req;free_req.path=src;free_req.time=0.2;free_req.width=320;free_req.height=180;free_req.token=175;free_req.allow_prepared_cache=false;free_req.effects=free_fx;free_req.effect_time=0.2;
    e.request_preview(free_req);Frame free_frame;std::uint64_t free_token=0;for(int i=0;i<300;++i){if(e.poll_preview(free_token,free_frame,err)&&free_token==175)break;std::this_thread::sleep_for(std::chrono::milliseconds(4));}
    req(free_token==175&&!free_frame.rgb.empty(),"free editing effects preview");
    std::uint64_t free_delta=0;for(std::size_t i=0;i<std::min(f.rgb.size(),free_frame.rgb.size());++i)free_delta+=static_cast<std::uint64_t>(std::abs(static_cast<int>(f.rgb[i])-static_cast<int>(free_frame.rgb[i])));req(free_delta>1000,"free editing effects change preview pixels");

    // Preview opacity is the final event/track/fade envelope over the black
    // preview background. This is also the path used by interactive fades.
    auto preview_at=[&](double opacity,std::uint64_t token){
        PreviewRequest r;r.path=src;r.time=0.42;r.width=320;r.height=180;r.token=token;r.allow_prepared_cache=false;r.opacity=opacity;e.request_preview(r);
        Frame out_frame;std::uint64_t out_token=0;std::string out_error;
        for(int i=0;i<250;++i){if(e.poll_preview(out_token,out_frame,out_error)&&out_token==token)break;std::this_thread::sleep_for(std::chrono::milliseconds(4));}
        req(out_token==token&&!out_frame.rgb.empty(),"opacity preview frame");return out_frame;
    };
    const Frame bright=preview_at(1.0,180),dim=preview_at(0.25,181);
    std::uint64_t bright_sum=0,dim_sum=0;for(auto v:bright.rgb)bright_sum+=v;for(auto v:dim.rgb)dim_sum+=v;
    req(bright_sum>0&&dim_sum<bright_sum/2,"preview fade opacity darkens frame");

    // Interactive FX editing at a stationary playhead reuses the decoded source
    // frame and only recomputes the lightweight in-process composite.
    e.clear_cache();const auto decodes_before=e.source_decode_count();
    PreviewRequest cache_a;cache_a.path=src;cache_a.time=0.33;cache_a.width=320;cache_a.height=180;cache_a.token=182;cache_a.allow_prepared_cache=false;cache_a.effects={fx_preview};cache_a.effect_time=0.2;e.request_preview(cache_a);Frame cache_frame;std::uint64_t cache_token=0;for(int i=0;i<250;++i){if(e.poll_preview(cache_token,cache_frame,err)&&cache_token==182)break;std::this_thread::sleep_for(std::chrono::milliseconds(4));}req(!cache_frame.rgb.empty(),"stationary FX source frame decode");
    set_effect_numeric(fx_preview,"brightness",0.22);PreviewRequest cache_b=cache_a;cache_b.token=183;cache_b.effects={fx_preview};e.clear_composite_cache();e.request_preview(cache_b);cache_frame={};cache_token=0;for(int i=0;i<250;++i){if(e.poll_preview(cache_token,cache_frame,err)&&cache_token==183)break;std::this_thread::sleep_for(std::chrono::milliseconds(4));}req(!cache_frame.rgb.empty(),"stationary FX source frame reuse");req(e.source_decode_count()-decodes_before==1,"source-frame cache avoids FFmpeg respawn while editing FX");

    std::optional<Frame> thumb;
    for(int i=0;i<250;++i){thumb=e.timeline_thumbnail(src,0.35,96,54);if(thumb&& !thumb->rgb.empty())break;std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    req(thumb.has_value()&&!thumb->rgb.empty(),"timeline thumbnail cache");

    e.prepare_media(*m);
    MediaPreparationProgress prep;
    for(int i=0;i<500;++i){prep=e.preparation_status(src);if(prep.ready)break;std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    req(prep.ready&&prep.progress>=0.999,"media preparation cache completes");
    req(!prep.stage.empty(),"media preparation reports stage");

    // Long-form preparation regression: sources beyond the cache threshold get
    // a persistent seek-friendly preview proxy instead of decoding the original
    // long-GOP media for every scrub request.
    const auto long_generated=run_process(e.ffmpeg_path(),{"-hide_banner","-loglevel","error","-y","-f","lavfi","-i","testsrc2=size=160x90:rate=24","-t","46","-c:v","libx264","-preset","ultrafast","-an",long_src});
    req(long_generated.started&&long_generated.exit_code==0,"generate long cache sample");
    auto long_media=e.probe(long_src,&err);req(long_media.has_value()&&long_media->has_video,"probe long cache sample");e.prepare_media(*long_media);
    MediaPreparationProgress long_prep;for(int i=0;i<3000;++i){long_prep=e.preparation_status(long_src);if(long_prep.ready)break;std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    req(long_prep.ready,"long media preparation completes");req(long_prep.proxy_ready,"long media optimized preview cache created");req(fs::exists(path_from_utf8(long_prep.proxy_path)),"long media proxy path exists");

    const auto still_generated=run_process(e.ffmpeg_path(),{"-hide_banner","-loglevel","error","-y","-f","lavfi","-i","color=c=0x3366cc:s=320x180","-frames:v","1",still});
    req(still_generated.started&&still_generated.exit_code==0,"generate still image");
    const auto odd_still_generated=run_process(e.ffmpeg_path(),{"-hide_banner","-loglevel","error","-y","-f","lavfi","-i","color=c=0xd83b92:s=1125x631","-frames:v","1",odd_still});
    req(odd_still_generated.started&&odd_still_generated.exit_code==0,"generate odd-size JPEG still image");
    const auto chroma_green_generated=run_process(e.ffmpeg_path(),{"-hide_banner","-loglevel","error","-y","-f","lavfi","-i","color=c=0x00ff00:s=160x90","-frames:v","1",chroma_green});
    const auto chroma_red_generated=run_process(e.ffmpeg_path(),{"-hide_banner","-loglevel","error","-y","-f","lavfi","-i","color=c=0xff0000:s=160x90","-frames:v","1",chroma_red});
    req(chroma_green_generated.started&&chroma_green_generated.exit_code==0&&chroma_red_generated.started&&chroma_red_generated.exit_code==0,"generate chroma-key stills");

    // A keyed pixel must become alpha and expose the lower layer.  The old
    // placeholder merely darkened green toward black and could not composite.
    auto chroma_fx=make_effect_instance("koralo.chroma_key",310);
    set_effect_numeric(chroma_fx,"color_r",0.0);set_effect_numeric(chroma_fx,"color_g",1.0);set_effect_numeric(chroma_fx,"color_b",0.0);
    set_effect_numeric(chroma_fx,"low_threshold",0.02);set_effect_numeric(chroma_fx,"high_threshold",0.18);set_effect_numeric(chroma_fx,"blur_amount",0.0);
    PreviewRequest chroma_req;chroma_req.path=chroma_green;chroma_req.time=0.0;chroma_req.width=160;chroma_req.height=90;chroma_req.token=187;chroma_req.continuous=false;chroma_req.allow_prepared_cache=false;chroma_req.effects={chroma_fx};
    chroma_req.has_background=true;chroma_req.background_path=chroma_red;chroma_req.background_time=0.0;chroma_req.background_allow_prepared_cache=false;
    e.request_preview(chroma_req);Frame chroma_frame;std::uint64_t chroma_token=0;err.clear();for(int i=0;i<300;++i){if(e.poll_preview(chroma_token,chroma_frame,err)&&chroma_token==187)break;std::this_thread::sleep_for(std::chrono::milliseconds(4));}
    if(chroma_frame.rgb.empty()&&!err.empty())std::cerr<<err;req(chroma_token==187&&!chroma_frame.rgb.empty(),"real chroma-key composite preview");
    std::uint64_t cr=0,cg=0,cb=0;for(std::size_t i=0;i+2<chroma_frame.rgb.size();i+=3){cr+=chroma_frame.rgb[i];cg+=chroma_frame.rgb[i+1];cb+=chroma_frame.rgb[i+2];}
    req(cr>cg*4&&cr>cb*4,"chroma key reveals lower red layer instead of blackening green");
    auto odd_still_media=e.probe(odd_still,&err);req(odd_still_media.has_value()&&odd_still_media->has_video&&!odd_still_media->has_audio,"probe odd-size JPEG still image");
    PreviewRequest odd_still_req;odd_still_req.path=odd_still;odd_still_req.time=3.0;odd_still_req.width=1125;odd_still_req.height=631;odd_still_req.token=189;odd_still_req.continuous=false;odd_still_req.allow_prepared_cache=false;odd_still_req.effects={make_effect_instance("koralo.anime_line_restore",198)};e.request_preview(odd_still_req);Frame odd_still_frame;std::uint64_t odd_still_token=0;err.clear();
    // This is a correctness regression, not a performance benchmark. Debug and
    // sanitizer builds can take several seconds on the full 1125x631 CPU Anime
    // filter, so allow enough time to observe completion while still failing a
    // genuinely stuck worker. Performance budgets belong in dedicated metrics.
    for(int i=0;i<1500;++i){if(e.poll_preview(odd_still_token,odd_still_frame,err)&&odd_still_token==189)break;std::this_thread::sleep_for(std::chrono::milliseconds(4));}if(odd_still_frame.rgb.empty()&&!err.empty())std::cerr<<err;req(odd_still_token==189&&!odd_still_frame.rgb.empty(),"odd-size JPEG still random-access preview with Anime FX");req(odd_still_frame.width==1125&&odd_still_frame.height==631,"odd-size JPEG preview dimensions");
    auto still_media=e.probe(still,&err);req(still_media.has_value()&&still_media->has_video&&!still_media->has_audio,"probe still image");req(still_media->duration>=4.99,"still image receives editable default duration");
    PreviewRequest still_req;still_req.path=still;still_req.time=2.0;still_req.width=320;still_req.height=180;still_req.token=190;still_req.continuous=true;still_req.requested_fps=24.0;still_req.allow_prepared_cache=false;e.request_preview(still_req);Frame still_frame;std::uint64_t still_token=0;for(int i=0;i<250;++i){if(e.poll_preview(still_token,still_frame,err)&&still_token==190)break;std::this_thread::sleep_for(std::chrono::milliseconds(4));}req(still_token==190&&!still_frame.rgb.empty(),"still image continuous preview");
    Project still_project=Project::make_default();int smid=still_project.add_media(*still_media).id;int svt=0;for(const auto&t:still_project.tracks)if(t.kind==TrackKind::Video){svt=t.id;break;}still_project.add_clip(smid,svt,0.0,0.0,1.2);err.clear();req(e.render_project(still_project,still_render,nullptr,&err),"render still image event");req(fs::exists(still_render_path)&&fs::file_size(still_render_path)>1000,"still image render file");

    auto green_media=e.probe(chroma_green,&err),red_media=e.probe(chroma_red,&err);req(green_media.has_value()&&red_media.has_value(),"probe chroma-key stills");
    Project chroma_project=Project::make_default();const int green_mid=chroma_project.add_media(*green_media).id,red_mid=chroma_project.add_media(*red_media).id;std::vector<int> video_tracks;for(const auto&t:chroma_project.tracks)if(t.kind==TrackKind::Video)video_tracks.push_back(t.id);req(video_tracks.size()>=2,"default project has two video layers");
    Clip& keyed=chroma_project.add_clip(green_mid,video_tracks[0],0.0,0.0,0.8);chroma_project.add_clip(red_mid,video_tracks[1],0.0,0.0,0.8);chroma_fx.id=chroma_project.next_id();keyed.video_effects.push_back(chroma_fx);
    err.clear();req(e.render_project(chroma_project,chroma_render,nullptr,&err),"render real chroma-key composite");if(!err.empty())std::cerr<<err;req(fs::exists(chroma_render_path)&&fs::file_size(chroma_render_path)>1000,"chroma-key render file");
    MediaEngine verify_engine;PreviewRequest chroma_render_req;chroma_render_req.path=chroma_render;chroma_render_req.time=0.2;chroma_render_req.width=160;chroma_render_req.height=90;chroma_render_req.token=188;chroma_render_req.allow_prepared_cache=false;verify_engine.request_preview(chroma_render_req);Frame chroma_render_frame;std::uint64_t chroma_render_token=0;err.clear();for(int i=0;i<600;++i){if(verify_engine.poll_preview(chroma_render_token,chroma_render_frame,err)&&chroma_render_token==188)break;std::this_thread::sleep_for(std::chrono::milliseconds(5));}if(chroma_render_frame.rgb.empty()&&!err.empty())std::cerr<<err;req(chroma_render_token==188&&!chroma_render_frame.rgb.empty(),"decode chroma-key final render");
    std::uint64_t rr=0,rg=0,rb=0;for(std::size_t i=0;i+2<chroma_render_frame.rgb.size();i+=3){rr+=chroma_render_frame.rgb[i];rg+=chroma_render_frame.rgb[i+1];rb+=chroma_render_frame.rgb[i+2];}req(rr>rg*3&&rr>rb*3,"final render keeps real keyed transparency over lower layer");

    // Native Titles & Text regression: generated media can preview without a
    // source file and render through the same project graph with transparent text.
    GeneratedText gt;gt.enabled=true;gt.text="KoraLo Text";gt.font_family="Open Sans";gt.font_size=46.0;gt.bold=true;gt.location_x=0.5;gt.location_y=0.5;gt.anchor=4;gt.outline=true;gt.shadow=true;
    PreviewRequest text_req;text_req.path="";text_req.time=0.0;text_req.width=320;text_req.height=180;text_req.token=195;text_req.has_text_overlay=true;text_req.text_overlay=gt;text_req.text_opacity=1.0;text_req.text_event_time=1.0;text_req.text_event_duration=5.0;
    e.request_preview(text_req);Frame text_frame;std::uint64_t text_token=0;for(int i=0;i<250;++i){if(e.poll_preview(text_token,text_frame,err)&&text_token==195)break;std::this_thread::sleep_for(std::chrono::milliseconds(4));}
    req(text_token==195&&!text_frame.rgb.empty(),"generated text preview frame");std::uint64_t text_sum=0;for(auto v:text_frame.rgb)text_sum+=v;req(text_sum>0,"generated text raster appears in preview");
    Project text_project=Project::make_default();MediaItem text_media;text_media.name="Titles & Text";text_media.duration=1.0;text_media.width=320;text_media.height=180;text_media.has_video=true;text_media.video_codec="koralo.generated.titles_text";text_media.generated_text=gt;const int tmid=text_project.add_media(text_media).id;int tvt=0;for(const auto&t:text_project.tracks)if(t.kind==TrackKind::Video){tvt=t.id;break;}text_project.add_clip(tmid,tvt,0.0,0.0,1.0);err.clear();req(e.render_project(text_project,text_render,nullptr,&err),"render generated text event");req(fs::exists(text_render_path)&&fs::file_size(text_render_path)>1000,"generated text render file");

    Project anime_project=Project::make_default();anime_project.settings.width=320;anime_project.settings.height=180;
    const int anime_mid=anime_project.add_media(*m).id;int anime_vt=0;for(const auto& t:anime_project.tracks)if(t.kind==TrackKind::Video){anime_vt=t.id;break;}
    Clip& anime_clip=anime_project.add_clip(anime_mid,anime_vt,0.0,0.0,0.8);
    for(const char* id:{"koralo.anime_line_restore","koralo.anime_vector_color","koralo.anime_text_restore","koralo.anime_paper_texture","koralo.anime_detail_sharpen","koralo.anime_deinterlace"})anime_clip.video_effects.push_back(make_effect_instance(id,anime_project.next_id()));
    err.clear();req(e.render_project(anime_project,anime_render,nullptr,&err),"render anime restore filter chain");req(fs::exists(anime_render_path)&&fs::file_size(anime_render_path)>1000,"anime restore render file");

    Project free_fx_project=Project::make_default();
    const int free_mid=free_fx_project.add_media(*m).id;int free_vt=0;for(const auto& t:free_fx_project.tracks)if(t.kind==TrackKind::Video){free_vt=t.id;break;}
    Clip& free_clip=free_fx_project.add_clip(free_mid,free_vt,0.0,0.0,0.8);
    for(const auto& fx:free_fx){EffectInstance copy=fx;copy.id=free_fx_project.next_id();free_clip.video_effects.push_back(std::move(copy));}
    err.clear();const bool free_fx_ok=e.render_project(free_fx_project,free_fx_render,nullptr,&err);if(!err.empty())std::cerr<<"Free effects render error: "<<err<<"\n";req(free_fx_ok,"render free editing effects chain");req(fs::exists(free_fx_render_path)&&fs::file_size(free_fx_render_path)>1000,"free editing effects render file");

    Project audio_fx_project=Project::make_default();
    const int audio_fx_mid=audio_fx_project.add_media(*m).id;int audio_fx_track=0;for(const auto& t:audio_fx_project.tracks)if(t.kind==TrackKind::Audio){audio_fx_track=t.id;break;}
    Clip& audio_fx_clip=audio_fx_project.add_clip(audio_fx_mid,audio_fx_track,0.0,0.0,0.8);
    for(const char* id:{"koralo.audio.echo","koralo.audio.reverb","koralo.audio.chorus","koralo.audio.flanger","koralo.audio.compressor","koralo.audio.limiter","koralo.audio.noise_gate","koralo.audio.eq3","koralo.audio.highpass","koralo.audio.lowpass","koralo.audio.distortion","koralo.audio.tremolo","koralo.audio.stereo_width","koralo.audio.gain"})audio_fx_clip.audio_effects.push_back(make_audio_effect_instance(id,audio_fx_project.next_id()));
    set_effect_numeric(audio_fx_clip.audio_effects[0],"wet",0.25);set_effect_numeric(audio_fx_clip.audio_effects[1],"wet",0.18);set_effect_numeric(audio_fx_clip.audio_effects[10],"mix",0.25);
    err.clear();req(e.render_project(audio_fx_project,audio_fx_render,nullptr,&err),"render native audio effects chain");if(!err.empty())std::cerr<<err;req(fs::exists(audio_fx_render_path)&&fs::file_size(audio_fx_render_path)>1000,"native audio effects render file");
    auto audio_fx_rendered=e.probe(audio_fx_render,&err);req(audio_fx_rendered.has_value(),"probe native audio FX render");req(audio_fx_rendered->duration<=0.86,"audio FX render is clamped to project duration");req(audio_fx_rendered->sample_rate==audio_fx_project.settings.sample_rate,"audio FX render follows project sample rate");

    Project p=Project::make_default();
    int mid=p.add_media(*m).id;int vt=0,at=0;
    for(auto&t:p.tracks){if(!vt&&t.kind==TrackKind::Video)vt=t.id;if(!at&&t.kind==TrackKind::Audio)at=t.id;}
    p.add_clip(mid,vt,0,0,0.5);p.add_clip(mid,at,0,0,0.5);
    AudioMeterLevel meter;
    for(int i=0;i<250;++i){meter=e.master_meter(p,0.2);if(meter.ready)break;std::this_thread::sleep_for(std::chrono::milliseconds(20));}
    req(meter.ready,"audio meter envelope ready");
    req(meter.left>0.001&&meter.right>0.001,"audio meter responds to real source level");
    if(Clip* audio_clip=p.clip_by_id(p.clips.back().id))audio_clip->fade_in=0.5;
    const auto faded_meter=e.master_meter(p,0.01);const auto open_meter=e.master_meter(p,0.4);
    req(faded_meter.ready&&open_meter.ready&&faded_meter.left<open_meter.left*0.2,"audio meter follows event fade gain");
    if(Clip* video_clip=p.clip_by_id(p.clips.front().id)){video_clip->fade_in=0.12;video_clip->fade_out=0.12;}
    req(e.render_project(p,out,nullptr,&err),"render");
    req(fs::exists(out_path)&&fs::file_size(out_path)>1000,"render file");

    // UI-control render regression: track/event opacity, overlap auto-crossfade, gain and pan must form a valid graph.
    if(Clip* fx_clip=p.clips.empty()?nullptr:&p.clips.front()){auto fx=make_effect_instance("koralo.brightness_contrast",p.next_id());set_effect_numeric(fx,"brightness",0.12);set_effect_numeric(fx,"contrast",0.18);fx_clip->video_effects.push_back(std::move(fx));}
    {auto fx=make_effect_instance("koralo.saturation",p.next_id());set_effect_numeric(fx,"amount",1.25);p.track_by_id(vt)->video_effects.push_back(std::move(fx));}
    {auto fx=make_effect_instance("koralo.gaussian_blur",p.next_id());set_effect_numeric(fx,"radius",0.35);p.video_output_effects.push_back(std::move(fx));}
    p.track_by_id(vt)->opacity=0.65;
    p.track_by_id(at)->gain=0.55;
    p.track_by_id(at)->pan=0.5;
    auto& overlap=p.add_clip(mid,vt,0.25,0.2,0.45);
    overlap.opacity=0.7;
    req(e.render_project(p,fx_out,nullptr,&err),"render controls graph");
    req(fs::exists(fx_out_path)&&fs::file_size(fx_out_path)>1000,"controls render file");

    // Animated Pan/Crop regression: the render graph must accept event-local
    // center/zoom/rotation keyframes and produce a valid video.
    if(Clip* motion_clip=p.clips.empty()?nullptr:&p.clips.front()){motion_clip->video_motion.set_key(0.0,0.5,0.5,1.0,0.0);motion_clip->video_motion.set_key(0.4,0.2,0.8,1.8,12.0);}
    err.clear();req(e.render_project(p,motion_out,nullptr,&err),"render animated pan crop graph");
    req(fs::exists(motion_out_path)&&fs::file_size(motion_out_path)>1000,"motion render file");

    RenderOptions preset;preset.width=640;preset.height=360;preset.fps=24.0;preset.video_bitrate_kbps=2500;preset.audio_bitrate_kbps=128;preset.use_gpu=false;preset.preset_name="Native Test Preset";
    err.clear();req(e.render_project(p,preset_out,nullptr,&err,&preset),"render editable preset");req(fs::exists(preset_out_path)&&fs::file_size(preset_out_path)>1000,"preset render file");
    auto rendered=e.probe(preset_out,&err);req(rendered.has_value()&&rendered->width==640&&rendered->height==360,"preset output dimensions");

    // Keep publishing when UI requests outrun effect processing. A seek or
    // pause invalidates the playback epoch and still wins over old frames.
    unsigned playback_frames=0;
    for(int i=0;i<200;++i){
        PreviewRequest advancing;advancing.path=src;advancing.time=.05+i/400.0;
        advancing.width=480;advancing.height=270;advancing.token=2000+i;
        advancing.continuous=true;advancing.playback=true;advancing.playback_epoch=123;
        advancing.requested_fps=30;advancing.allow_prepared_cache=false;
        advancing.effects={make_effect_instance("koralo.anime_restore_v45",456)};
        e.request_preview(advancing);std::this_thread::sleep_for(std::chrono::milliseconds(3));
        std::uint64_t token=0;Frame ready;std::string error;
        if(e.poll_preview(token,ready,error)&&token>=2000&&!ready.rgb.empty())++playback_frames;
    }
    req(playback_frames>0,"slow effect playback publishes frames while requests advance");
    PreviewRequest seek;seek.path=src;seek.time=.2;seek.width=320;seek.height=180;
    seek.token=3000;seek.playback_epoch=124;seek.allow_prepared_cache=false;
    e.request_preview(seek);Frame sought;std::uint64_t seek_token=0;err.clear();
    for(int i=0;i<500;++i){if(e.poll_preview(seek_token,sought,err)&&seek_token==3000)break;std::this_thread::sleep_for(std::chrono::milliseconds(4));}
    req(seek_token==3000&&!sought.rgb.empty(),"paused seek supersedes previous playback epoch");
    req(sought.width==320&&sought.height==180,"paused seek restores requested preview size");

    // Font family is part of the title-frame cache; old Open Sans projects
    // retain their glyphs while new titles use the embedded Inter atlas.
    Frame title_frames[2];
    for(int family=0;family<2;++family){
        PreviewRequest title;title.width=320;title.height=180;title.token=3010+family;
        title.has_text_overlay=true;title.text_overlay.enabled=true;title.text_overlay.text="Title / Заголовок";
        title.text_overlay.font_family=family?"Open Sans":"Inter";
        e.request_preview(title);std::uint64_t token=0;err.clear();
        for(int i=0;i<500;++i){if(e.poll_preview(token,title_frames[family],err)&&token==title.token)break;std::this_thread::sleep_for(std::chrono::milliseconds(4));}
        req(token==title.token&&!title_frames[family].rgb.empty(),"embedded title typeface decodes");
    }
    req(title_frames[0].rgb!=title_frames[1].rgb,"title cache separates Inter and legacy Open Sans");

    fs::remove(src_path,ec);ec.clear();
    fs::remove(out_path,ec);ec.clear();
    fs::remove(fx_out_path,ec);ec.clear();
    fs::remove(motion_out_path,ec);ec.clear();
    fs::remove(preset_out_path,ec);ec.clear();
    fs::remove(long_src_path,ec);ec.clear();
    fs::remove(still_path,ec);ec.clear();
    fs::remove(odd_still_path,ec);ec.clear();
    fs::remove(chroma_green_path,ec);ec.clear();
    fs::remove(chroma_red_path,ec);ec.clear();
    fs::remove(chroma_render_path,ec);ec.clear();
    fs::remove(still_render_path,ec);ec.clear();
    fs::remove(text_render_path,ec);ec.clear();
    fs::remove(anime_render_path,ec);ec.clear();
    fs::remove(free_fx_render_path,ec);ec.clear();
    fs::remove(audio_fx_render_path,ec);ec.clear();
    fs::remove(raw_record_path,ec);ec.clear();
    fs::remove(wav_record_path,ec);ec.clear();
    if(!long_prep.proxy_path.empty())fs::remove(path_from_utf8(long_prep.proxy_path),ec);ec.clear();
    fs::remove(dir,ec);
    std::cout<<"KoraLo native media tests passed\n";
    return 0;
}
