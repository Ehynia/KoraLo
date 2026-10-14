#include "core/edit_controller.hpp"
#include "core/project.hpp"
#include "core/vegas_importer.hpp"
#include "platform/process.hpp"
#include "ui/localization.hpp"
#include "plugins/ofx_host.hpp"
#include "plugins/ofx_ipc.hpp"
#include <cmath>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace koralo;

static void req(bool ok, const char* msg) { if (!ok) throw std::runtime_error(msg); }
static bool near(double a, double b) { return std::abs(a - b) < 1e-9; }

struct Fixture {
    Project p = Project::make_default();
    int media = 0;
    int video_track = 0;
    int audio_track = 0;
    int group = 0;
    int video = 0;
    int audio = 0;

    Fixture() {
        MediaItem m;
        m.path = "sample.mp4";
        m.name = "sample";
        m.duration = 10;
        m.has_video = true;
        m.has_audio = true;
        m.width = 1920;
        m.height = 1080;
        m.fps = 30;
        media = p.add_media(m).id;
        for (auto& t : p.tracks) {
            if (!video_track && t.kind == TrackKind::Video) video_track = t.id;
            if (!audio_track && t.kind == TrackKind::Audio) audio_track = t.id;
        }
        group = p.next_group_id();
        video = p.add_clip(media, video_track, 1, 0, 5, group).id;
        audio = p.add_clip(media, audio_track, 1, 0, 5, group).id;
    }
};

static TimelineGeometry geometry() {
    TimelineGeometry g;
    g.x = 0;
    g.y = 0;
    g.header_w = 180;
    g.ruler_h = 30;
    g.px_per_sec = 100;
    return g;
}


static void test_fade_handle_transaction() {
    Fixture f;
    EditController e;
    UndoStack undo;
    TimelineGeometry g = geometry();
    const int y = g.y + g.ruler_h + 20;
    const Clip* before_video = f.p.clip_by_id(f.video);
    req(before_video != nullptr, "fade setup video");
    const int right_handle = e.x_at(g, before_video->end());

    e.begin_clip_drag(f.p, g, f.video, right_handle, y, DragMode::FadeOut, false);
    e.update_drag(f.p, g, right_handle - 200, y, 0.0);
    req(near(f.p.clip_by_id(f.video)->fade_out, 0.0), "fade preview does not mutate project");
    req(e.drag.preview.contains(f.video) && near(e.drag.preview.at(f.video).fade_out, 2.0), "fade out preview follows mouse");
    req(e.commit_drag(f.p, undo, g, right_handle - 200, y, 0.0), "fade out commit");
    req(near(f.p.clip_by_id(f.video)->fade_out, 2.0), "fade out committed");
    req(near(f.p.clip_by_id(f.audio)->fade_out, 0.0), "fade edit is per event");
    req(f.p.clip_by_id(f.video)->group_id == f.group && f.p.clip_by_id(f.audio)->group_id == f.group, "fade preserves linked A/V group");
    req(undo.undo(f.p), "fade out undo");
    req(near(f.p.clip_by_id(f.video)->fade_out, 0.0), "fade out undo restores event");

    const int left_handle = e.x_at(g, f.p.clip_by_id(f.video)->start);
    e.begin_clip_drag(f.p, g, f.video, left_handle, y, DragMode::FadeIn, false);
    e.update_drag(f.p, g, left_handle + 150, y, 0.0);
    req(e.commit_drag(f.p, undo, g, left_handle + 150, y, 0.0), "fade in commit");
    req(near(f.p.clip_by_id(f.video)->fade_in, 1.5), "fade in committed");
}

static void test_project_and_undo() {
    Fixture f;
    req(near(f.p.duration(), 6.0), "duration");
    int right = 0;
    req(f.p.split_clip(f.video, 3, &right), "split");
    req(right != 0, "right id");
    req(near(f.p.clip_by_id(f.video)->duration, 2.0), "left duration");

    UndoStack undo;
    EditController e;
    e.selection.select_clip(f.p, right, SelectionOperation::Replace, false);
    req(e.nudge_selected(f.p, undo, 1.0), "nudge");
    req(near(f.p.clip_by_id(right)->start, 4.0), "nudged start");
    req(undo.undo(f.p), "undo");
    req(near(f.p.clip_by_id(right)->start, 3.0), "undo start");

    auto tmp_path = std::filesystem::temp_directory_path() / path_from_utf8("koralo project тест.kora");
    auto tmp = path_to_utf8(tmp_path);
    req(f.p.save(tmp), "save unicode path");
    std::string err;
    auto loaded = Project::load(tmp, &err);
    req(loaded.has_value(), "load");
    req(loaded->clips.size() == f.p.clips.size(), "roundtrip clips");
    std::filesystem::remove(tmp_path);
}

static void test_project_semantic_roundtrip() {
    Project p=Project::make_default();
    p.settings.width=2560;p.settings.height=1440;p.settings.fps=59.94;p.settings.sample_rate=96000;
    p.settings.preview_quality=3;p.settings.preview_fps_limit=48;p.settings.use_prepared_cache=false;
    p.snapping=false;p.auto_crossfade=false;p.ripple=RippleMode::AllTracks;
    p.ignore_event_grouping=true;p.slip_all_takes=true;p.quantize_to_frames=false;

    Track* video_track=nullptr;Track* audio_track=nullptr;
    for(auto& track:p.tracks){
        if(!video_track&&track.kind==TrackKind::Video)video_track=&track;
        if(!audio_track&&track.kind==TrackKind::Audio)audio_track=&track;
    }
    req(video_track&&audio_track,"semantic roundtrip default tracks");
    const int video_track_id=video_track->id,audio_track_id=audio_track->id;
    video_track->name="Video Ω";video_track->mute=true;video_track->locked=true;video_track->opacity=.73;video_track->height=91;
    video_track->video_effects.push_back(make_effect_instance("koralo.brightness_contrast",p.next_id()));
    audio_track->name="Audio Ω";audio_track->solo=true;audio_track->gain=.61;audio_track->pan=-.35;audio_track->height=83;
    audio_track->audio_effects.push_back(make_audio_effect_instance("koralo.audio.echo",p.next_id()));
    audio_track->imported_audio_fx={"Legacy EQ","Legacy Reverb"};

    MediaItem text_media;text_media.name="Generated Text";text_media.duration=8;text_media.width=2560;text_media.height=1440;text_media.fps=59.94;text_media.has_video=true;
    text_media.generated_text.enabled=true;text_media.generated_text.text="KoraLo\nRound trip";text_media.generated_text.font_family="Inter";text_media.generated_text.font_size=67;
    text_media.generated_text.bold=true;text_media.generated_text.italic=true;text_media.generated_text.alignment=2;text_media.generated_text.location_x=.27;text_media.generated_text.location_y=.68;
    text_media.generated_text.outline=true;text_media.generated_text.outline_size=3.25;text_media.generated_text.shadow=true;text_media.generated_text.shadow_opacity=.42;text_media.generated_text.animation=1;
    const int text_media_id=p.add_media(std::move(text_media)).id;

    MediaItem vector_media;vector_media.name="Generated Vector";vector_media.duration=6;vector_media.width=2560;vector_media.height=1440;vector_media.fps=59.94;vector_media.has_video=true;
    vector_media.generated_vector.enabled=true;vector_media.generated_vector.crop_enabled=true;vector_media.generated_vector.crop_x.default_value=.1;vector_media.generated_vector.crop_w.default_value=.8;
    auto shape=make_vector_rectangle(1);shape.name="Shape Ω";shape.x.default_value=.4;shape.opacity.default_value=.8;shape.x.set_key(1.0,.7,KeyframeInterpolation::Hold);
    vector_media.generated_vector.shapes.push_back(shape);vector_media.generated_vector.next_shape_id=2;
    const auto expected_vector_fingerprint=vector_scene_fingerprint(vector_media.generated_vector);
    const int vector_media_id=p.add_media(std::move(vector_media)).id;

    const int group=p.next_group_id();
    Clip& clip=p.add_clip(text_media_id,video_track_id,2.5,.75,4.0,group);
    const int clip_id=clip.id;clip.rate=1.25;clip.gain=.8;clip.opacity=.66;clip.fade_in=.4;clip.fade_out=.7;clip.snap_offset=.2;clip.muted=true;clip.loop=true;
    clip.video_motion.enabled=true;clip.video_motion.center_x.default_value=.4;clip.video_motion.center_y.default_value=.6;clip.video_motion.zoom.default_value=1.5;clip.video_motion.rotation.default_value=-12;
    clip.video_motion.center_x.set_key(.5,.2,KeyframeInterpolation::Hold);
    clip.video_effects.push_back(make_effect_instance("koralo.brightness_contrast",p.next_id()));
    clip.audio_effects.push_back(make_audio_effect_instance("koralo.audio.echo",p.next_id()));
    req(p.add_take(clip_id,vector_media_id,1.25,"Vector take",true),"semantic roundtrip alternate take");
    p.video_output_effects.push_back(make_effect_instance("koralo.brightness_contrast",p.next_id()));
    p.markers.push_back({3.25,"Marker Ω"});

    const auto first_path=std::filesystem::temp_directory_path()/path_from_utf8("koralo semantic roundtrip 1.kora");
    const auto second_path=std::filesystem::temp_directory_path()/path_from_utf8("koralo semantic roundtrip 2.kora");
    req(p.save(path_to_utf8(first_path)),"semantic roundtrip first save");
    std::string error;auto first=Project::load(path_to_utf8(first_path),&error);req(first.has_value(),"semantic roundtrip first load");
    req(first->save(path_to_utf8(second_path)),"semantic roundtrip second save");
    auto second=Project::load(path_to_utf8(second_path),&error);req(second.has_value(),"semantic roundtrip second load");

    const Project& q=*second;
    req(q.settings.width==2560&&q.settings.height==1440&&near(q.settings.fps,59.94)&&q.settings.sample_rate==96000,"semantic roundtrip project settings");
    req(q.settings.preview_quality==3&&q.settings.preview_fps_limit==48&&!q.settings.use_prepared_cache,"semantic roundtrip preview settings");
    req(!q.snapping&&!q.auto_crossfade&&q.ripple==RippleMode::AllTracks&&q.ignore_event_grouping&&q.slip_all_takes&&!q.quantize_to_frames,"semantic roundtrip edit flags");
    const Track* qv=q.track_by_id(video_track_id);const Track* qa=q.track_by_id(audio_track_id);
    req(qv&&qv->name=="Video Ω"&&qv->mute&&qv->locked&&near(qv->opacity,.73)&&qv->height==91,"semantic roundtrip video track");
    req(qa&&qa->name=="Audio Ω"&&qa->solo&&near(qa->gain,.61)&&near(qa->pan,-.35)&&qa->height==83,"semantic roundtrip audio track");
    req(qv->video_effects.size()==1&&qa->audio_effects.size()==1&&qa->imported_audio_fx.size()==2,"semantic roundtrip track effects");
    const MediaItem* qt=q.media_by_id(text_media_id);const MediaItem* qvec=q.media_by_id(vector_media_id);
    req(qt&&qt->generated_text.enabled&&qt->generated_text.text=="KoraLo\nRound trip"&&near(qt->generated_text.font_size,67),"semantic roundtrip generated text");
    req(qvec&&qvec->generated_vector.enabled&&vector_scene_fingerprint(qvec->generated_vector)==expected_vector_fingerprint,"semantic roundtrip vector scene");
    const Clip* qc=q.clip_by_id(clip_id);req(qc&&qc->takes.size()==2&&qc->active_take==1&&qc->media_id()==vector_media_id,"semantic roundtrip clip takes");
    req(near(qc->start,2.5)&&near(qc->duration,4.0)&&near(qc->rate,1.25)&&near(qc->opacity,.66)&&near(qc->fade_in,.4)&&near(qc->fade_out,.7)&&near(qc->snap_offset,.2)&&qc->muted&&qc->loop,"semantic roundtrip clip fields");
    req(qc->video_motion.enabled&&near(qc->video_motion.zoom.default_value,1.5)&&qc->video_motion.center_x.keys.size()==1,"semantic roundtrip motion");
    req(qc->video_effects.size()==1&&qc->audio_effects.size()==1&&q.video_output_effects.size()==1,"semantic roundtrip event/output effects");
    req(q.markers.size()==1&&near(q.markers[0].time,3.25)&&q.markers[0].label=="Marker Ω","semantic roundtrip marker");
    std::filesystem::remove(first_path);std::filesystem::remove(second_path);
}

static void test_project_load_rejects_duplicate_settings_and_generator_conflicts() {
    const auto duplicate_path=std::filesystem::temp_directory_path()/path_from_utf8("koralo duplicate settings.kora");
    {
        std::ofstream f(duplicate_path,std::ios::binary);
        f<<"KORALO\t15\n";
        f<<"SETTINGS\t1920\t1080\t30\t48000\t1\t1\t0\t0\t0\t1\t1\t30\t1\n";
        f<<"SETTINGS\t1280\t720\t25\t44100\t0\t0\t1\t0\t0\t1\t0\t25\t0\n";
    }
    std::string error;auto duplicate=Project::load(path_to_utf8(duplicate_path),&error);
    req(!duplicate&&!error.empty(),"duplicate project settings are rejected");
    std::filesystem::remove(duplicate_path);

    const auto conflict_path=std::filesystem::temp_directory_path()/path_from_utf8("koralo generated media conflict.kora");
    {
        std::ofstream f(conflict_path,std::ios::binary);
        f<<"KORALO\t15\n";
        f<<"SETTINGS\t1920\t1080\t30\t48000\t1\t1\t0\t0\t0\t1\t1\t30\t1\n";
        f<<"MEDIA\t1\t\tGenerated\t5\t1920\t1080\t30\t1\t0\t0\t0\t\t\t\n";
        f<<"GTEXT\t1\tText\tInter\t48\t0\t0\t1\t1\t1\t1\t1\t1\t.5\t.5\t4\t0\t2\t0\t0\t0\t1\t0\t4\t4\t.55\t0\n";
        f<<"VANIM\t1\tinvalid-because-owner-is-already-text\n";
    }
    error.clear();auto conflict=Project::load(path_to_utf8(conflict_path),&error);
    req(!conflict&&!error.empty(),"conflicting generated media records are rejected");
    std::filesystem::remove(conflict_path);
}

static void test_group_aware_selection() {
    Fixture f;
    EditController e;
    e.selection.select_clip(f.p, f.video);
    req(e.selection.primary_clip() == f.video, "selection primary");
    req(e.selection.contains(f.video), "video selected");
    req(e.selection.contains(f.audio), "linked audio selected");
    req(e.selection.clips().size() == 2, "group selected exactly");
}


static void test_detach_linked_audio_group() {
    Fixture f;
    req(f.p.clip_by_id(f.video)->group_id == f.group && f.p.clip_by_id(f.audio)->group_id == f.group, "detach setup linked");
    UndoStack undo;
    undo.begin(f.p);
    req(f.p.detach_event_group(f.video), "detach linked audio group");
    req(undo.commit(f.p), "detach commit");
    req(f.p.clip_by_id(f.video)->group_id == 0 && f.p.clip_by_id(f.audio)->group_id == 0, "detach clears both group ids");
    req(undo.undo(f.p), "detach undo");
    req(f.p.clip_by_id(f.video)->group_id == f.group && f.p.clip_by_id(f.audio)->group_id == f.group, "detach undo restores link");
}

static void test_ignore_event_grouping_selection() {
    Fixture f;
    EditController e;
    f.p.ignore_event_grouping = true;
    e.selection.select_clip(f.p, f.video);
    req(e.selection.contains(f.video), "ignore grouping keeps clicked event");
    req(!e.selection.contains(f.audio), "ignore grouping does not expand linked event");
    req(e.selection.clips().size() == 1, "ignore grouping selection size");
}

static void test_take_model_and_roundtrip() {
    Fixture f;
    MediaItem alt;
    alt.path = "alternate.mp4";
    alt.name = "alternate";
    alt.duration = 12.0;
    alt.has_video = true;
    alt.has_audio = true;
    const int alt_media = f.p.add_media(alt).id;
    req(f.p.add_take(f.video, alt_media, 1.25, "Alt take", true), "add alternate take");
    Clip* clip = f.p.clip_by_id(f.video);
    req(clip && clip->takes.size() == 2, "two takes stored");
    req(clip->active_take == 1 && clip->media_id() == alt_media, "active take controls media");
    req(near(clip->source_offset(), 1.25), "active take controls source offset");

    auto tmp_path = std::filesystem::temp_directory_path() / path_from_utf8("koralo takes тест.kora");
    const auto tmp = path_to_utf8(tmp_path);
    req(f.p.save(tmp), "save take project");
    std::string error;
    auto loaded = Project::load(tmp, &error);
    req(loaded.has_value(), "load take project");
    const Clip* loaded_clip = loaded->clip_by_id(f.video);
    req(loaded_clip && loaded_clip->takes.size() == 2, "take roundtrip count");
    req(loaded_clip->active_take == 1 && loaded_clip->media_id() == alt_media, "take roundtrip active");
    req(near(loaded_clip->source_offset(), 1.25), "take roundtrip offset");
    std::filesystem::remove(tmp_path);
}

static void test_v5_project_backward_load() {
    const auto tmp_path = std::filesystem::temp_directory_path() / path_from_utf8("koralo legacy v5 тест.kora");
    {
        std::ofstream f(tmp_path, std::ios::binary);
        f << "KORALO\t5\n";
        f << "SETTINGS\t1920\t1080\t30\t48000\t1\t1\t0\n";
        f << "MEDIA\t1\tlegacy.mp4\tlegacy\t10\t1920\t1080\t30\t1\t0\t0\t0\th264\t\t\n";
        f << "TRACK\t2\tV\tVideo 1\t0\t0\t0\t1\t0\t1\t76\n";
        f << "CLIP\t3\t1\t2\t0\t2\t4\t1.5\t1\t1\t1\t0\t0\t0\t0\n";
    }
    std::string error;
    auto loaded = Project::load(path_to_utf8(tmp_path), &error);
    req(loaded.has_value(), "legacy v5 project loads");
    const Clip* clip = loaded->clip_by_id(3);
    req(clip && clip->takes.size() == 1, "legacy media/in point migrated to take");
    req(clip->media_id() == 1 && near(clip->source_offset(), 1.5), "legacy take data preserved");
    std::filesystem::remove(tmp_path);
}

static void test_group_aware_split_creates_right_group() {
    Fixture f;
    MediaItem alt;
    alt.path = "split-alt.mp4"; alt.name = "split-alt"; alt.duration = 20.0; alt.has_video = true;
    const int alt_media = f.p.add_media(alt).id;
    req(f.p.add_take(f.video, alt_media, 3.0, "Split alt", false), "split alternate take setup");
    f.p.clip_by_id(f.video)->fade_in = 0.5;
    f.p.clip_by_id(f.video)->fade_out = 0.75;
    EditController e;
    UndoStack undo;
    e.selection.select_clip(f.p, f.video, SelectionOperation::Replace, true);
    req(e.split_selected(f.p, undo, 3.0), "group split");
    req(f.p.clips.size() == 4, "group split creates two right events");

    int right_group = 0;
    int right_count = 0;
    for (const auto& clip : f.p.clips) {
        if (!near(clip.start, 3.0)) continue;
        ++right_count;
        if (!right_group) right_group = clip.group_id;
        req(clip.group_id == right_group, "right split pair shares new group");
        req(near(clip.source_offset(), 2.0), "split advances source offset");
    }
    req(right_count == 2, "both linked members split");
    req(right_group > 0 && right_group != f.group, "right side gets independent group");
    req(f.p.clip_by_id(f.video)->group_id == f.group && f.p.clip_by_id(f.audio)->group_id == f.group, "left side keeps original group");
    req(near(f.p.clip_by_id(f.video)->fade_in, 0.5) && near(f.p.clip_by_id(f.video)->fade_out, 0.0), "split keeps only outer left fades");
    for (const auto& clip : f.p.clips) if (near(clip.start, 3.0) && clip.track_id == f.video_track) {
        req(near(clip.fade_in, 0.0) && near(clip.fade_out, 0.75), "split keeps only outer right fades");
        req(clip.takes.size() == 2 && near(clip.takes[1].offset, 5.0), "split advances every take offset");
    }
    req(undo.undo(f.p), "group split undo");
    req(f.p.clips.size() == 2, "group split undo restores pair");
}

static void test_split_without_selection() {
    Fixture f;
    EditController e;
    UndoStack undo;
    req(e.selection.empty(), "split cursor starts unselected");
    req(e.split_selected(f.p, undo, 3.0), "split cursor without selection");
    req(f.p.clips.size() == 4, "cursor split affects intersecting linked events");
}

static void test_slip_active_and_all_takes() {
    Fixture f;
    MediaItem alt;
    alt.path = "alternate.mp4";
    alt.name = "alternate";
    alt.duration = 20.0;
    alt.has_video = true;
    const int alt_media = f.p.add_media(alt).id;
    req(f.p.add_take(f.video, alt_media, 4.0, "Alt", false), "add second take for slip");
    Clip* clip = f.p.clip_by_id(f.video);
    req(clip && clip->takes.size() == 2, "slip take setup");

    EditController e;
    UndoStack undo;
    TimelineGeometry g = geometry();
    const int y = g.y + g.ruler_h + 20;
    const int x = e.x_at(g, 2.0);
    f.p.ignore_event_grouping = true;
    e.begin_clip_drag(f.p, g, f.video, x, y, DragMode::Slip, false);
    e.update_drag(f.p, g, x + 100, y, 0.0);
    req(e.commit_drag(f.p, undo, g, x + 100, y, 0.0), "active-take slip commit");
    clip = f.p.clip_by_id(f.video);
    req(near(clip->takes[0].offset, 1.0), "active take slipped");
    req(near(clip->takes[1].offset, 4.0), "inactive take preserved");

    f.p.slip_all_takes = true;
    const int x2 = e.x_at(g, 2.0);
    e.begin_clip_drag(f.p, g, f.video, x2, y, DragMode::Slip, false);
    e.update_drag(f.p, g, x2 + 100, y, 0.0);
    req(e.commit_drag(f.p, undo, g, x2 + 100, y, 0.0), "all-takes slip commit");
    clip = f.p.clip_by_id(f.video);
    req(near(clip->takes[0].offset, 2.0), "active take slipped again");
    req(near(clip->takes[1].offset, 5.0), "inactive take follows slip-all");
}

static void test_take_cycle_and_loop_undo() {
    Fixture f;
    MediaItem alt;
    alt.path = "alternate.mp4";
    alt.name = "alternate";
    alt.duration = 10.0;
    alt.has_video = true;
    const int alt_media = f.p.add_media(alt).id;
    req(f.p.add_take(f.video, alt_media, 2.0, "Alt", false), "cycle setup take");
    EditController e;
    UndoStack undo;
    f.p.ignore_event_grouping = true;
    e.selection.select_clip(f.p, f.video);
    req(e.cycle_active_take(f.p, undo, 1), "cycle active take");
    req(f.p.clip_by_id(f.video)->active_take == 1, "take cycle state");
    req(undo.undo(f.p), "take cycle undo");
    req(f.p.clip_by_id(f.video)->active_take == 0, "take cycle undo state");
    req(e.toggle_loop_selected(f.p, undo), "event loop toggle");
    req(f.p.clip_by_id(f.video)->loop, "event loop enabled");
    req(undo.undo(f.p), "event loop undo");
    req(!f.p.clip_by_id(f.video)->loop, "event loop undo state");
}

static void test_quantize_to_frames_move() {
    Fixture f;
    f.p.settings.fps = 10.0;
    f.p.snapping = false;
    f.p.quantize_to_frames = true;
    f.p.ignore_event_grouping = true;
    EditController e;
    UndoStack undo;
    TimelineGeometry g = geometry();
    const int y = g.y + g.ruler_h + 20;
    const int x = e.x_at(g, 1.5);
    e.begin_clip_drag(f.p, g, f.video, x, y, DragMode::Move, false);
    e.update_drag(f.p, g, x + 6, y, 0.0); // +0.06s -> nearest 0.1s frame at 10fps.
    req(near(e.drag.preview[f.video].start, 1.1), "move quantized to frame");
    req(e.commit_drag(f.p, undo, g, x + 6, y, 0.0), "quantized move commit");
    req(near(f.p.clip_by_id(f.video)->start, 1.1), "quantized move state");
}

static void test_trim_start_end_to_cursor() {
    Fixture f;
    EditController e;
    UndoStack undo;
    e.selection.select_clip(f.p, f.video, SelectionOperation::Replace, true);
    req(e.trim_start_to_cursor(f.p, undo, 2.0), "trim start to cursor");
    req(near(f.p.clip_by_id(f.video)->start, 2.0) && near(f.p.clip_by_id(f.video)->duration, 4.0), "video trim start state");
    req(near(f.p.clip_by_id(f.audio)->start, 2.0) && near(f.p.clip_by_id(f.audio)->duration, 4.0), "audio trim start state");
    req(near(f.p.clip_by_id(f.video)->source_offset(), 1.0), "trim start advances take offset");
    req(undo.undo(f.p), "trim start undo");
    e.selection.select_clip(f.p, f.video, SelectionOperation::Replace, true);
    req(e.trim_end_to_cursor(f.p, undo, 5.0), "trim end to cursor");
    req(near(f.p.clip_by_id(f.video)->duration, 4.0) && near(f.p.clip_by_id(f.audio)->duration, 4.0), "trim end group state");
    req(near(f.p.clip_by_id(f.video)->source_offset(), 0.0), "trim end keeps source offset");
}

static void test_group_vertical_move_preserves_kind_offset() {
    Fixture f;
    const int video2 = [&]{ for (const auto& t : f.p.tracks) if (t.kind == TrackKind::Video && t.id != f.video_track) return t.id; return 0; }();
    const int audio2 = f.p.add_track(TrackKind::Audio, "Audio 2").id;
    req(video2 != 0 && audio2 != 0, "vertical move destination tracks");
    EditController e;
    UndoStack undo;
    TimelineGeometry g = geometry();
    e.selection.select_clip(f.p, f.video, SelectionOperation::Replace, true);
    const int x = e.x_at(g, 2.0);
    int y = g.y + g.ruler_h;
    int target_y = y;
    for (const auto& track : f.p.tracks) {
        if (track.id == video2) { target_y = y + track.height / 2; break; }
        y += track.height;
    }
    const int source_y = g.y + g.ruler_h + 20;
    e.begin_clip_drag(f.p, g, f.video, x, source_y, DragMode::Move, false);
    e.update_drag(f.p, g, x, target_y, 0.0);
    req(e.drag.preview[f.video].track_id == video2, "video group member moves to next video track");
    req(e.drag.preview[f.audio].track_id == audio2, "audio group member moves to next audio track");
    req(e.commit_drag(f.p, undo, g, x, target_y, 0.0), "vertical group move commit");
    req(f.p.clip_by_id(f.video)->track_id == video2 && f.p.clip_by_id(f.audio)->track_id == audio2, "vertical group move state");
    req(undo.undo(f.p), "vertical group move undo");
    req(f.p.clip_by_id(f.video)->track_id == f.video_track && f.p.clip_by_id(f.audio)->track_id == f.audio_track, "vertical group move undo state");
}

static void test_marquee_group_selection() {
    Fixture f;
    const int standalone = f.p.add_clip(f.media, f.video_track, 8.0, 0.0, 1.0).id;
    EditController e;
    TimelineGeometry g = geometry();

    // Rectangle touches only the video member of the A/V group; group-aware selection must include audio too.
    const int x1 = e.x_at(g, 1.2);
    const int x2 = e.x_at(g, 2.0);
    const int y1 = g.y + g.ruler_h + 8;
    const int y2 = g.y + g.ruler_h + 40;
    e.begin_marquee(f.p, x1, y1, false);
    e.update_drag(f.p, g, x2, y2, 0.0);
    req(e.selection.contains(f.video), "marquee video selected");
    req(e.selection.contains(f.audio), "marquee linked audio selected");
    req(!e.selection.contains(standalone), "marquee excludes distant clip");

    // Additive marquee must preserve the existing group and add another clip.
    const int sx1 = e.x_at(g, 8.1);
    const int sx2 = e.x_at(g, 8.8);
    e.begin_marquee(f.p, sx1, y1, true);
    e.update_drag(f.p, g, sx2, y2, 0.0);
    req(e.selection.contains(f.video) && e.selection.contains(f.audio), "additive marquee preserves group");
    req(e.selection.contains(standalone), "additive marquee adds clip");
}

static void test_transactional_move_preview() {
    Fixture f;
    EditController e;
    TimelineGeometry g = geometry();
    const int x = e.x_at(g, f.p.clip_by_id(f.video)->start + 0.5);
    const int y = g.y + g.ruler_h + 20;
    int edge = 0;
    req(e.hit_clip(f.p, g, x, y, &edge) == f.video, "hit test");

    e.begin_clip_drag(f.p, g, f.video, x, y, DragMode::None, false);
    e.update_drag(f.p, g, x + 100, y, 0.0);
    req(near(f.p.clip_by_id(f.video)->start, 1.0), "drag mutated model before drop");
    req(near(e.drag.preview[f.video].start, 2.0), "fake drag preview");

    UndoStack undo;
    req(e.commit_drag(f.p, undo, g, x + 100, y, 0.0), "commit drag");
    req(near(f.p.clip_by_id(f.video)->start, 2.0), "drag commit");
    req(near(f.p.clip_by_id(f.audio)->start, 2.0), "group move commit");
}

static void test_group_aware_trim_transaction() {
    Fixture f;
    EditController e;
    TimelineGeometry g = geometry();
    UndoStack undo;
    const int y = g.y + g.ruler_h + 20;

    // Right-edge trim by one second. Neither linked event may mutate before commit.
    const int edge_x = e.x_at(g, f.p.clip_by_id(f.video)->end());
    e.begin_clip_drag(f.p, g, f.video, edge_x, y, DragMode::None, false);
    req(e.drag.mode == DragMode::TrimRight, "right trim mode");
    e.update_drag(f.p, g, edge_x - 100, y, 0.0);
    req(near(f.p.clip_by_id(f.video)->duration, 5.0), "trim mutated video before drop");
    req(near(f.p.clip_by_id(f.audio)->duration, 5.0), "trim mutated audio before drop");
    req(near(e.drag.preview[f.video].duration, 4.0), "video trim preview");
    req(near(e.drag.preview[f.audio].duration, 4.0), "audio trim preview");

    req(e.commit_drag(f.p, undo, g, edge_x - 100, y, 0.0), "trim commit");
    req(near(f.p.clip_by_id(f.video)->duration, 4.0), "video trim commit");
    req(near(f.p.clip_by_id(f.audio)->duration, 4.0), "audio trim commit");
    req(undo.undo(f.p), "trim undo");
    req(near(f.p.clip_by_id(f.video)->duration, 5.0) && near(f.p.clip_by_id(f.audio)->duration, 5.0), "trim undo state");
    req(undo.redo(f.p), "trim redo");
    req(near(f.p.clip_by_id(f.video)->duration, 4.0) && near(f.p.clip_by_id(f.audio)->duration, 4.0), "trim redo state");
}

static void test_multi_selection_move_transaction() {
    Fixture f;
    const int standalone = f.p.add_clip(f.media, f.video_track, 7.0, 0.0, 1.0).id;
    EditController e;
    TimelineGeometry g = geometry();
    UndoStack undo;
    const int y = g.y + g.ruler_h + 20;

    e.selection.select_clip(f.p, f.video, SelectionOperation::Replace, true);
    e.selection.select_clip(f.p, standalone, SelectionOperation::Add, true);
    const int body_x = e.x_at(g, f.p.clip_by_id(f.video)->start + 0.5);
    e.begin_clip_drag(f.p, g, f.video, body_x, y, DragMode::None, false);
    e.update_drag(f.p, g, body_x + 50, y, 0.0);
    req(near(e.drag.preview[f.video].start, 1.5), "multi move video preview");
    req(near(e.drag.preview[f.audio].start, 1.5), "multi move linked audio preview");
    req(near(e.drag.preview[standalone].start, 7.5), "multi move standalone preview");
    req(near(f.p.clip_by_id(standalone)->start, 7.0), "multi move mutated before commit");

    req(e.commit_drag(f.p, undo, g, body_x + 50, y, 0.0), "multi move commit");
    req(near(f.p.clip_by_id(f.video)->start, 1.5), "multi move video commit");
    req(near(f.p.clip_by_id(f.audio)->start, 1.5), "multi move audio commit");
    req(near(f.p.clip_by_id(standalone)->start, 7.5), "multi move standalone commit");
    req(undo.undo(f.p), "multi move undo");
    req(near(f.p.clip_by_id(f.video)->start, 1.0) && near(f.p.clip_by_id(standalone)->start, 7.0), "multi move undo state");
    req(undo.redo(f.p), "multi move redo");
    req(near(f.p.clip_by_id(f.video)->start, 1.5) && near(f.p.clip_by_id(standalone)->start, 7.5), "multi move redo state");
}

static Project sequence_project(int& media_id, int& track_id, int& a, int& b, int& c) {
    Project p = Project::make_default();
    MediaItem m;
    m.path = "sequence.mp4";
    m.name = "sequence";
    m.duration = 30.0;
    m.has_video = true;
    media_id = p.add_media(m).id;
    for (auto& track : p.tracks) if (track.kind == TrackKind::Video) { track_id = track.id; break; }
    a = p.add_clip(media_id, track_id, 0.0, 0.0, 5.0).id;
    b = p.add_clip(media_id, track_id, 5.0, 5.0, 4.0).id;
    c = p.add_clip(media_id, track_id, 9.0, 9.0, 5.0).id;
    return p;
}

static void test_roll_transaction() {
    int media=0, track=0, a=0, b=0, c=0;
    Project p = sequence_project(media, track, a, b, c);
    EditController e;
    UndoStack undo;
    TimelineGeometry g = geometry();
    const int y = g.y + g.ruler_h + 20;
    const int edge_x = e.x_at(g, 5.0);

    e.begin_clip_drag(p, g, a, edge_x, y, DragMode::Roll, false);
    req(e.drag.mode == DragMode::Roll, "roll mode");
    e.update_drag(p, g, edge_x + 100, y, 0.0);
    req(near(p.clip_by_id(a)->duration, 5.0) && near(p.clip_by_id(b)->start, 5.0), "roll mutated before commit");
    req(near(e.drag.preview[a].duration, 6.0), "roll left preview");
    req(near(e.drag.preview[b].start, 6.0) && near(e.drag.preview[b].duration, 3.0), "roll right preview");
    req(near(e.drag.preview[b].in_point, 6.0), "roll source preview");
    req(e.commit_drag(p, undo, g, edge_x + 100, y, 0.0), "roll commit");
    req(near(p.clip_by_id(a)->end(), p.clip_by_id(b)->start), "roll boundary joined");
    req(near(p.clip_by_id(b)->end(), 9.0), "roll keeps outer end");
    req(undo.undo(p), "roll undo");
    req(near(p.clip_by_id(a)->duration, 5.0) && near(p.clip_by_id(b)->start, 5.0), "roll undo state");
}

static void test_slide_transaction() {
    int media=0, track=0, a=0, b=0, c=0;
    Project p = sequence_project(media, track, a, b, c);
    EditController e;
    UndoStack undo;
    TimelineGeometry g = geometry();
    const int y = g.y + g.ruler_h + 20;
    const int body_x = e.x_at(g, 7.0);

    e.begin_clip_drag(p, g, b, body_x, y, DragMode::Slide, false);
    req(e.drag.mode == DragMode::Slide, "slide mode");
    e.update_drag(p, g, body_x + 100, y, 0.0);
    req(near(p.clip_by_id(b)->start, 5.0), "slide mutated before commit");
    req(near(e.drag.preview[a].duration, 6.0), "slide left preview");
    req(near(e.drag.preview[b].start, 6.0) && near(e.drag.preview[b].in_point, 5.0), "slide event preview");
    req(near(e.drag.preview[c].start, 10.0) && near(e.drag.preview[c].duration, 4.0), "slide right preview");
    req(e.commit_drag(p, undo, g, body_x + 100, y, 0.0), "slide commit");
    req(near(p.clip_by_id(a)->end(), p.clip_by_id(b)->start), "slide left joined");
    req(near(p.clip_by_id(b)->end(), p.clip_by_id(c)->start), "slide right joined");
    req(near(p.clip_by_id(c)->end(), 14.0), "slide keeps outer end");
    req(undo.undo(p), "slide undo");
    req(near(p.clip_by_id(b)->start, 5.0) && near(p.clip_by_id(c)->start, 9.0), "slide undo state");
}

static void test_ripple_trim_transaction() {
    int media=0, track=0, a=0, b=0, c=0;
    Project p = sequence_project(media, track, a, b, c);
    EditController e;
    UndoStack undo;
    TimelineGeometry g = geometry();
    const int y = g.y + g.ruler_h + 20;
    const int edge_x = e.x_at(g, 5.0);

    e.begin_clip_drag(p, g, a, edge_x, y, DragMode::RippleTrim, false);
    req(e.drag.mode == DragMode::RippleTrim, "ripple trim mode");
    e.update_drag(p, g, edge_x - 100, y, 0.0);
    req(near(p.clip_by_id(a)->duration, 5.0) && near(p.clip_by_id(b)->start, 5.0), "ripple trim mutated before commit");
    req(near(e.drag.preview[a].duration, 4.0), "ripple trim event preview");
    req(near(e.drag.preview[b].start, 4.0) && near(e.drag.preview[c].start, 8.0), "ripple trim downstream preview");
    req(e.commit_drag(p, undo, g, edge_x - 100, y, 0.0), "ripple trim commit");
    req(near(p.clip_by_id(a)->end(), p.clip_by_id(b)->start), "ripple trim closes first boundary");
    req(near(p.duration(), 13.0), "ripple trim changes project duration");
    req(undo.undo(p), "ripple trim undo");
    req(near(p.duration(), 14.0) && near(p.clip_by_id(b)->start, 5.0), "ripple trim undo state");
    req(undo.redo(p), "ripple trim redo");
    req(near(p.duration(), 13.0), "ripple trim redo state");
}

static void test_track_reorder_transaction() {
    Project p = Project::make_default();
    const int first = p.tracks.front().id;
    const int second = p.add_track(TrackKind::Video, "Video 2").id;
    const int third = p.add_track(TrackKind::Video, "Video 3").id;
    (void)second;
    EditController e;
    UndoStack undo;
    TimelineGeometry g = geometry();

    int target_y = g.y + g.ruler_h;
    int expected_index = -1;
    for (std::size_t i = 0; i < p.tracks.size(); ++i) {
        const auto& track = p.tracks[i];
        if (track.id == third) { target_y += track.height / 2; expected_index = static_cast<int>(i); break; }
        target_y += track.height;
    }
    req(expected_index >= 0, "track reorder expected index");
    e.begin_track_reorder(p, g, first, 20, g.y + g.ruler_h + 20);
    e.update_drag(p, g, 20, target_y, 0.0);
    req(p.tracks.front().id == first, "track reorder mutated before commit");
    req(e.drag.target_track_index >= 0, "track reorder preview target");
    req(e.commit_drag(p, undo, g, 20, target_y, 0.0), "track reorder commit");
    req(p.tracks[static_cast<std::size_t>(expected_index)].id == first, "track reorder final index");
    req(undo.undo(p), "track reorder undo");
    req(p.tracks.front().id == first, "track reorder undo state");
    req(undo.redo(p), "track reorder redo");
    req(p.tracks[static_cast<std::size_t>(expected_index)].id == first, "track reorder redo state");
}

static void test_clipboard_group_transaction() {
    Fixture f;
    EditController e;
    UndoStack undo;
    ClipClipboard clipboard;
    e.selection.select_clip(f.p, f.video, SelectionOperation::Replace, true);
    req(e.copy_selected(f.p, clipboard), "copy selection");
    req(clipboard.clips.size() == 2, "copy linked pair");
    req(e.paste(f.p, undo, clipboard, 8.0), "paste selection");
    req(f.p.clips.size() == 4, "paste clip count");
    int pasted_group = 0;
    int pasted_count = 0;
    for (const auto& clip : f.p.clips) {
        if (near(clip.start, 8.0)) {
            ++pasted_count;
            if (!pasted_group) pasted_group = clip.group_id;
            req(clip.group_id == pasted_group, "pasted pair grouped");
        }
    }
    req(pasted_count == 2 && pasted_group > 0 && pasted_group != f.group, "paste remaps group");
    req(undo.undo(f.p), "paste undo");
    req(f.p.clips.size() == 2, "paste undo state");
    req(undo.redo(f.p), "paste redo");
    req(f.p.clips.size() == 4, "paste redo state");
}

static void test_event_lock_blocks_edit() {
    Fixture f;
    EditController e;
    UndoStack undo;
    TimelineGeometry g = geometry();
    e.selection.select_clip(f.p, f.video, SelectionOperation::Replace, true);
    req(e.toggle_lock_selected(f.p, undo), "lock selection");
    req(f.p.clip_by_id(f.video)->locked && f.p.clip_by_id(f.audio)->locked, "group locked");
    const int x = e.x_at(g, 2.0);
    const int y = g.y + g.ruler_h + 20;
    e.begin_clip_drag(f.p, g, f.video, x, y, DragMode::None, false);
    req(e.drag.mode == DragMode::None, "locked event drag blocked");
    req(!e.delete_selected(f.p, undo), "locked delete blocked");
    req(e.toggle_lock_selected(f.p, undo), "unlock selection");
    req(!f.p.clip_by_id(f.video)->locked && !f.p.clip_by_id(f.audio)->locked, "group unlocked");
}

static void test_trimmer_insert_overwrite_transactions() {
    {
        Fixture f;
        EditController e;
        UndoStack undo;
        req(e.insert_media(f.p, undo, f.media, 2.0, 4.0, 3.0), "insert media");
        req(f.p.clips.size() == 6, "insert split and pair count");
        int inserted = 0, shifted_right = 0;
        for (const auto& clip : f.p.clips) {
            if (near(clip.start, 3.0) && near(clip.duration, 2.0) && near(clip.source_offset(), 2.0)) ++inserted;
            if (near(clip.start, 5.0) && near(clip.duration, 3.0)) ++shifted_right;
        }
        req(inserted == 2, "insert creates linked source range");
        req(shifted_right == 2, "insert ripples split right pair");
        req(undo.undo(f.p), "insert undo");
        req(f.p.clips.size() == 2 && near(f.p.clip_by_id(f.video)->start, 1.0) && near(f.p.clip_by_id(f.video)->duration, 5.0), "insert undo state");
        req(undo.redo(f.p), "insert redo");
        req(f.p.clips.size() == 6, "insert redo state");
    }
    {
        Fixture f;
        EditController e;
        UndoStack undo;
        req(e.overwrite_media(f.p, undo, f.media, 0.0, 2.0, 2.0), "overwrite media");
        req(f.p.clips.size() == 6, "overwrite split and pair count");
        int inserted = 0, lefts = 0, rights = 0;
        for (const auto& clip : f.p.clips) {
            if (near(clip.start, 2.0) && near(clip.duration, 2.0) && near(clip.source_offset(), 0.0)) ++inserted;
            if (near(clip.start, 1.0) && near(clip.duration, 1.0)) ++lefts;
            if (near(clip.start, 4.0) && near(clip.duration, 2.0) && near(clip.source_offset(), 3.0)) ++rights;
        }
        req(inserted == 2 && lefts == 2 && rights == 2, "overwrite preserves outside material");
        req(undo.undo(f.p), "overwrite undo");
        req(f.p.clips.size() == 2 && near(f.p.clip_by_id(f.video)->duration, 5.0), "overwrite undo state");
    }
}

static void test_control_state_undo() {
    Fixture f;
    UndoStack undo;
    Track* video_track = f.p.track_by_id(f.video_track);
    Track* audio_track = f.p.track_by_id(f.audio_track);
    req(video_track && audio_track, "control state tracks");

    undo.begin(f.p);
    f.p.settings.width = 3840;
    f.p.settings.height = 2160;
    f.p.settings.fps = 59.94;
    f.p.settings.sample_rate = 96000;
    f.p.snapping = !f.p.snapping;
    f.p.auto_crossfade = !f.p.auto_crossfade;
    f.p.ripple = RippleMode::AllTracks;
    f.p.markers.push_back({2.5, "UI marker"});
    video_track->mute = true;
    video_track->solo = true;
    video_track->locked = true;
    video_track->opacity = 0.55;
    audio_track->gain = 0.45;
    audio_track->pan = -0.35;
    f.p.clip_by_id(f.video)->opacity = 0.6;
    req(undo.commit(f.p), "control state transaction committed");

    req(undo.undo(f.p), "control state undo");
    video_track = f.p.track_by_id(f.video_track);
    audio_track = f.p.track_by_id(f.audio_track);
    req(video_track && !video_track->mute && !video_track->solo && !video_track->locked, "track buttons undo");
    req(f.p.settings.width == 1920 && f.p.settings.height == 1080 && near(f.p.settings.fps, 30.0) && f.p.settings.sample_rate == 48000, "project settings undo");
    req(near(video_track->opacity, 1.0), "track opacity undo");
    req(near(audio_track->gain, 1.0) && near(audio_track->pan, 0.0), "audio controls undo");
    req(near(f.p.clip_by_id(f.video)->opacity, 1.0), "event opacity undo");
    req(f.p.markers.empty(), "marker undo");

    req(undo.redo(f.p), "control state redo");
    video_track = f.p.track_by_id(f.video_track);
    audio_track = f.p.track_by_id(f.audio_track);
    req(video_track->mute && video_track->solo && video_track->locked, "track buttons redo");
    req(f.p.settings.width == 3840 && f.p.settings.height == 2160 && near(f.p.settings.fps, 59.94) && f.p.settings.sample_rate == 96000, "project settings redo");
    req(near(video_track->opacity, 0.55), "track opacity redo");
    req(near(audio_track->gain, 0.45) && near(audio_track->pan, -0.35), "audio controls redo");
    req(f.p.markers.size() == 1, "marker redo");
}


static void test_video_motion_keyframes_and_split() {
    VideoMotion implicit;implicit.set_key(2.0,0.25,0.75,2.0,10.0);
    req(implicit.has_key(0.0) && implicit.has_key(2.0), "first motion edit creates baseline key");
    req(near(sample_motion(implicit,0.0).zoom,1.0), "baseline uses default motion");
    VideoMotion motion;
    motion.set_key(0.0, 0.25, 0.30, 1.0, 0.0);
    motion.set_key(2.0, 0.75, 0.70, 2.0, 20.0);
    const auto mid = sample_motion(motion, 1.0);
    req(near(mid.center_x, 0.5) && near(mid.center_y, 0.5), "motion center interpolation");
    req(near(mid.zoom, 1.5) && near(mid.rotation, 10.0), "motion zoom/rotation interpolation");

    VideoMotion right;
    split_motion(motion, right, 1.0);
    req(motion.has_key(1.0) && right.has_key(0.0), "motion split boundary keys");
    const auto left_end = sample_motion(motion, 1.0);
    const auto right_start = sample_motion(right, 0.0);
    req(near(left_end.center_x, right_start.center_x) && near(left_end.zoom, right_start.zoom), "motion split continuity");
    const auto right_end = sample_motion(right, 1.0);
    req(near(right_end.center_x, 0.75) && near(right_end.zoom, 2.0), "motion split shifts later keyframes");
}

static void test_video_motion_project_roundtrip_and_undo() {
    Fixture f;
    Clip* clip = f.p.clip_by_id(f.video);
    req(clip != nullptr, "motion project clip");
    UndoStack undo;
    undo.begin(f.p);
    clip->video_motion.set_key(0.0, 0.5, 0.5, 1.0, 0.0);
    clip->video_motion.set_key(2.0, 0.2, 0.8, 2.5, -15.0);
    req(undo.commit(f.p), "motion creates undo transaction");
    req(undo.undo(f.p), "motion undo");
    req(!f.p.clip_by_id(f.video)->video_motion.enabled, "motion undo state");
    req(undo.redo(f.p), "motion redo");
    clip = f.p.clip_by_id(f.video);
    req(clip->video_motion.enabled && clip->video_motion.has_key(2.0), "motion redo state");

    auto tmp_path = std::filesystem::temp_directory_path() / path_from_utf8("koralo motion тест.kora");
    const auto tmp = path_to_utf8(tmp_path);
    req(f.p.save(tmp), "save motion project");
    std::string error; auto loaded = Project::load(tmp, &error);
    req(loaded.has_value(), "load motion project");
    const Clip* loaded_clip = loaded->clip_by_id(f.video);
    req(loaded_clip && loaded_clip->video_motion.enabled, "motion roundtrip enabled");
    const auto at = sample_motion(loaded_clip->video_motion, 2.0);
    req(near(at.center_x, 0.2) && near(at.center_y, 0.8) && near(at.zoom, 2.5) && near(at.rotation, -15.0), "motion roundtrip values");
    std::filesystem::remove(tmp_path);
}


static void test_effect_model_roundtrip_and_undo() {
    Fixture f;
    Clip* clip=f.p.clip_by_id(f.video);req(clip!=nullptr,"effect clip");
    UndoStack undo;undo.begin(f.p);
    EffectInstance fx=make_effect_instance("koralo.brightness_contrast",f.p.next_id());
    req(set_effect_numeric(fx,"brightness",0.25),"set effect brightness");
    auto* contrast=effect_parameter(fx,"contrast");req(contrast!=nullptr,"contrast param");contrast->set_numeric_key(0.0,0.0);contrast->set_numeric_key(2.0,0.5);
    clip->video_effects.push_back(fx);req(undo.commit(f.p),"effect undo transaction");
    req(near(effect_numeric(clip->video_effects.front(),"contrast",1.0,0.0),0.25),"effect key interpolation");
    req(undo.undo(f.p),"effect undo");req(f.p.clip_by_id(f.video)->video_effects.empty(),"effect undo state");req(undo.redo(f.p),"effect redo");
    auto tmp_path=std::filesystem::temp_directory_path()/path_from_utf8("koralo effects тест.kora");const auto tmp=path_to_utf8(tmp_path);req(f.p.save(tmp),"save effects project");
    std::string error;auto loaded=Project::load(tmp,&error);req(loaded.has_value(),"load effects project");const Clip* lc=loaded->clip_by_id(f.video);req(lc&&lc->video_effects.size()==1,"effect roundtrip count");
    req(near(effect_numeric(lc->video_effects.front(),"brightness",0.0,0.0),0.25),"effect roundtrip value");req(near(effect_numeric(lc->video_effects.front(),"contrast",1.0,0.0),0.25),"effect roundtrip key");
    const char* anime_ids[]={"koralo.anime_line_restore","koralo.anime_vector_color","koralo.anime_text_restore","koralo.anime_paper_texture","koralo.anime_detail_sharpen","koralo.anime_deinterlace"};
    for(const char* id:anime_ids){const auto* def=effect_definition(id);req(def!=nullptr,"anime restore effect definition");req(def->category=="Anime Restore","anime restore effect category");auto inst=make_effect_instance(id,123);req(inst.definition_id==id,"anime restore effect instance");}
    const char* free_fx_ids[]={"koralo.glow","koralo.hue_shift","koralo.levels","koralo.vignette","koralo.pixelate","koralo.lens_distortion","koralo.rgb_split","koralo.letterbox","koralo.scanlines"};
    for(const char* id:free_fx_ids){const auto* def=effect_definition(id);req(def!=nullptr,"free editing effect definition");auto inst=make_effect_instance(id,321);req(inst.definition_id==id,"free editing effect instance");}
    std::filesystem::remove(tmp_path);
}

static void test_vegas_riff_inspection() {
    const auto tmp_path=std::filesystem::temp_directory_path()/path_from_utf8("koralo vegas inspect тест.veg");
    std::vector<unsigned char> bytes(40,0);
    const unsigned char riff[16]={0x72,0x69,0x66,0x66,0x2e,0x91,0xcf,0x11,0xa5,0xd6,0x28,0xdb,0x04,0xc1,0x00,0x00};
    const unsigned char form[16]={0xef,0x29,0xc4,0x46,0x4a,0x90,0xd2,0x11,0x87,0x22,0x00,0xc0,0x4f,0x8e,0xdb,0x8a};
    const unsigned char settings_guid[16]={0x5a,0x2d,0x8f,0xb2,0x0f,0x23,0xd2,0x11,0x86,0xaf,0x00,0xc0,0x4f,0x8e,0xdb,0x8a};
    std::copy(riff,riff+16,bytes.begin());std::copy(form,form+16,bytes.begin()+24);
    const std::size_t chunk=bytes.size();bytes.resize(chunk+24+0x164,0);std::copy(settings_guid,settings_guid+16,bytes.begin()+static_cast<std::ptrdiff_t>(chunk));
    auto put64=[&](std::size_t off,std::uint64_t v){std::memcpy(bytes.data()+off,&v,sizeof(v));};
    auto put32=[&](std::size_t off,std::int32_t v){std::memcpy(bytes.data()+off,&v,sizeof(v));};
    auto putdouble=[&](std::size_t off,double v){std::memcpy(bytes.data()+off,&v,sizeof(v));};
    put64(chunk+16,24+0x164);const std::size_t payload=chunk+24;put32(payload+0x0c,48000);putdouble(payload+0x10,29.97002997);put32(payload+0x15c,1920);put32(payload+0x160,1080);
    const std::u16string media=u"C:\\Media\\clean-room sample.mp3";for(char16_t ch:media){bytes.push_back(static_cast<unsigned char>(ch&0xff));bytes.push_back(static_cast<unsigned char>((ch>>8)&0xff));}bytes.push_back(0);bytes.push_back(0);
    put64(16,bytes.size());
    {std::ofstream f(tmp_path,std::ios::binary);f.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));}
    std::string error;auto info=inspect_vegas_project(path_to_utf8(tmp_path),&error);req(info.has_value(),"inspect synthetic VEG container");
    req(info->has_settings,"VEG settings recovered");req(info->settings.width==1920&&info->settings.height==1080,"VEG dimensions recovered");req(near(info->settings.fps,29.97002997),"VEG fps recovered");req(info->settings.sample_rate==48000,"VEG sample rate recovered");
    req(info->media_paths.size()==1&&info->media_paths.front().find("clean-room sample.mp3")!=std::string::npos,"VEG media path recovered");
    std::filesystem::remove(tmp_path);
}



static void test_vegas12_settings_and_wmv_inspection() {
    const auto tmp_path=std::filesystem::temp_directory_path()/path_from_utf8("koralo vegas12 settings test.veg");
    std::vector<unsigned char> bytes(40,0);
    const unsigned char riff[16]={0x72,0x69,0x66,0x66,0x2e,0x91,0xcf,0x11,0xa5,0xd6,0x28,0xdb,0x04,0xc1,0x00,0x00};
    const unsigned char form[16]={0xef,0x29,0xc4,0x46,0x4a,0x90,0xd2,0x11,0x87,0x22,0x00,0xc0,0x4f,0x8e,0xdb,0x8a};
    const unsigned char settings_guid[16]={0x5a,0x2d,0x8f,0xb2,0x0f,0x23,0xd2,0x11,0x86,0xaf,0x00,0xc0,0x4f,0x8e,0xdb,0x8a};
    std::copy(riff,riff+16,bytes.begin());std::copy(form,form+16,bytes.begin()+24);
    const std::size_t chunk=bytes.size();bytes.resize(chunk+24+0x532,0);std::copy(settings_guid,settings_guid+16,bytes.begin()+static_cast<std::ptrdiff_t>(chunk));
    auto put64=[&](std::size_t off,std::uint64_t v){std::memcpy(bytes.data()+off,&v,sizeof(v));};
    auto put32=[&](std::size_t off,std::int32_t v){std::memcpy(bytes.data()+off,&v,sizeof(v));};
    auto putdouble=[&](std::size_t off,double v){std::memcpy(bytes.data()+off,&v,sizeof(v));};
    put64(chunk+16,24+0x532);const std::size_t payload=chunk+24;put32(payload+0x0c,48000);putdouble(payload+0x10,23.976);put32(payload+0x52a,1920);put32(payload+0x52e,1080);
    const std::u16string media=u"C:\\Media\\blue.wmv";for(char16_t ch:media){bytes.push_back(static_cast<unsigned char>(ch&0xff));bytes.push_back(static_cast<unsigned char>((ch>>8)&0xff));}bytes.push_back(0);bytes.push_back(0);
    put64(16,bytes.size());{std::ofstream f(tmp_path,std::ios::binary);f.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));}
    std::string error;auto info=inspect_vegas_project(path_to_utf8(tmp_path),&error);req(info.has_value(),"inspect synthetic VEGAS 12 container");
    req(info->has_settings&&info->settings.width==1920&&info->settings.height==1080,"VEGAS 12 settings layout recovered");
    req(near(info->settings.fps,23.976)&&info->settings.sample_rate==48000,"VEGAS 12 frame/audio settings recovered");
    req(info->media_paths.size()==1&&info->media_paths.front().find("blue.wmv")!=std::string::npos,"WMV reference recovered from VEG media pool");
    std::filesystem::remove(tmp_path);
}

static void test_timeline_vertical_scroll_hit_testing() {
    Project p=Project::make_default();
    req(p.tracks.size()>=2,"default tracks for vertical scroll");
    EditController e;TimelineGeometry g;g.x=0;g.y=100;g.width=1000;g.height=300;g.header_w=260;g.ruler_h=24;g.scroll_y=0;
    const int y=g.y+g.ruler_h+10;
    req(e.track_at(p,g,y)==p.tracks[0].id,"unscrolled first track hit");
    g.scroll_y=p.tracks[0].height;
    req(e.track_at(p,g,y)==p.tracks[1].id,"vertical scroll hit follows visible track");
}

static void test_timeline_vertical_scrollbar_geometry() {
    Project p=Project::make_default();
    req(p.tracks.size()>=2,"default tracks for vertical scrollbar geometry");
    p.tracks.resize(2);
    p.tracks[0].height=100;
    p.tracks[1].height=100;

    TimelineGeometry g;
    g.x=0;g.y=100;g.width=1000;g.height=124;g.header_w=260;g.ruler_h=24;g.scroll_y=0;
    auto metrics=timeline_vertical_scroll_metrics(p,g);
    req(metrics.visible(),"vertical scrollbar appears when content exceeds viewport");
    req(metrics.content_height==200&&metrics.viewport_height==100,"vertical scrollbar uses track content and body viewport heights");
    req(metrics.max_scroll==100,"vertical scrollbar max scroll matches hidden content");
    req(metrics.track_height==96&&metrics.thumb_height==48&&metrics.travel==48,"vertical thumb size follows viewport/content ratio");
    req(timeline_vertical_scroll_from_thumb_top(metrics,metrics.track_y)==0,"vertical scrollbar maps top to zero scroll");
    req(timeline_vertical_scroll_from_thumb_top(metrics,metrics.track_y+metrics.travel)==metrics.max_scroll,"vertical scrollbar maps bottom to max scroll");

    g.scroll_y=50;
    metrics=timeline_vertical_scroll_metrics(p,g);
    req(std::abs(metrics.thumb_y-(metrics.track_y+metrics.travel/2))<=1,"vertical thumb follows scroll position");
    req(std::abs(timeline_vertical_scroll_from_thumb_top(metrics,metrics.thumb_y)-50)<=1,"vertical drag mapping round trips scroll position");
}

static void test_timeline_horizontal_scroll_keeps_header_noninteractive() {
    Fixture f;
    EditController e;
    TimelineGeometry g=geometry();
    g.width=900;
    g.scroll_sec=2.5;
    const int y=g.y+g.ruler_h+20;

    // The event starts at 1s, so after scrolling to 2.5s its raw left edge is
    // inside the fixed header pane. The visible continuation must still be
    // hittable in the content area, but the hidden portion over the header
    // must not steal track-header input.
    req(e.x_at(g,f.p.clip_by_id(f.video)->start)<g.x+g.header_w,"scrolled event edge reaches behind header");
    req(e.hit_clip(f.p,g,g.x+g.header_w-20,y)==0,"scrolled event cannot be hit through track header");
    req(e.hit_clip(f.p,g,g.x+g.header_w+20,y)==f.video,"visible scrolled event remains hittable");
}

static void test_vegas_event_timing_inspection() {
    using Bytes = std::vector<unsigned char>;
    using Guid = std::array<unsigned char,16>;
    const Guid riff{0x72,0x69,0x66,0x66,0x2e,0x91,0xcf,0x11,0xa5,0xd6,0x28,0xdb,0x04,0xc1,0x00,0x00};
    const Guid form{0xef,0x29,0xc4,0x46,0x4a,0x90,0xd2,0x11,0x87,0x22,0x00,0xc0,0x4f,0x8e,0xdb,0x8a};
    const Guid list{0x6c,0x69,0x73,0x74,0x2f,0x91,0xcf,0x11,0xa5,0xd6,0x28,0xdb,0x04,0xc1,0x00,0x00};
    const Guid tracks_type{0x47,0x07,0x6c,0x4d,0x16,0x23,0xd2,0x11,0x86,0xb0,0x00,0xc0,0x4f,0x8e,0xdb,0x8a};
    const Guid track_type{0x48,0x07,0x6c,0x4d,0x16,0x23,0xd2,0x11,0x86,0xb0,0x00,0xc0,0x4f,0x8e,0xdb,0x8a};
    const Guid track_header{0x49,0x07,0x6c,0x4d,0x16,0x23,0xd2,0x11,0x86,0xb0,0x00,0xc0,0x4f,0x8e,0xdb,0x8a};
    const Guid event_type{0x4a,0x07,0x6c,0x4d,0x16,0x23,0xd2,0x11,0x86,0xb0,0x00,0xc0,0x4f,0x8e,0xdb,0x8a};
    const Guid event_meta{0x6a,0x20,0x8d,0x16,0x21,0x23,0xd2,0x11,0x86,0xb0,0x00,0xc0,0x4f,0x8e,0xdb,0x8a};
    const Guid take_meta{0x6d,0x20,0x8d,0x16,0x21,0x23,0xd2,0x11,0x86,0xb0,0x00,0xc0,0x4f,0x8e,0xdb,0x8a};
    auto put64=[](Bytes& v,std::size_t off,std::uint64_t n){std::memcpy(v.data()+off,&n,sizeof(n));};
    auto put32=[](Bytes& v,std::size_t off,std::uint32_t n){std::memcpy(v.data()+off,&n,sizeof(n));};
    auto chunk=[&](const Guid& id,const Bytes& payload){Bytes out;out.insert(out.end(),id.begin(),id.end());out.resize(24);put64(out,16,24+payload.size());out.insert(out.end(),payload.begin(),payload.end());while(out.size()%8)out.push_back(0);return out;};
    auto list_chunk=[&](const Guid& type,const std::vector<Bytes>& kids){Bytes payload(type.begin(),type.end());for(const auto& kid:kids)payload.insert(payload.end(),kid.begin(),kid.end());return chunk(list,payload);};
    auto make_track=[&](std::uint32_t raw_kind,double start,double duration,double source_offset){
        Bytes th(56,0);put32(th,28,raw_kind);Bytes th_leaf=chunk(track_header,th);
        Bytes em(64,0);put64(em,0x10,static_cast<std::uint64_t>(std::llround(start*10000000.0)));put64(em,0x18,static_cast<std::uint64_t>(std::llround(duration*10000000.0)));
        Bytes tm(40,0);put64(tm,0x10,static_cast<std::uint64_t>(std::llround(source_offset*10000000.0)));put64(tm,0x18,static_cast<std::uint64_t>(std::llround(duration*10000000.0)));
        const std::u16string name=u"transformers-remix";for(char16_t ch:name){tm.push_back(static_cast<unsigned char>(ch&0xff));tm.push_back(static_cast<unsigned char>(ch>>8));}tm.push_back(0);tm.push_back(0);
        auto put_utf16=[&](const std::u16string& text){for(char16_t ch:text){tm.push_back(static_cast<unsigned char>(ch&0xff));tm.push_back(static_cast<unsigned char>(ch>>8));}tm.push_back(0);tm.push_back(0);};
        auto put_double=[&](double value){const auto old=tm.size();tm.resize(old+sizeof(value));std::memcpy(tm.data()+old,&value,sizeof(value));};
        const std::u16string fx=u"{Svfx:com.sonycreativesoftware:chromakeyer}";put_utf16(fx);
        // Clean-room synthetic copy of the named-value layout observed in the
        // supplied VEGAS project: UTF-16 parameter name followed immediately
        // by its scalar/RGB value slot.  This locks the importer to evidence we
        // can reproduce without depending on proprietary implementation code.
        put_utf16(u"color");put_double(0.16);put_double(0.95);put_double(0.09);
        put_utf16(u"LowThreshold");put_double(0.07);
        put_utf16(u"HighThreshold");put_double(0.31);
        put_utf16(u"BlurAmount");put_double(0.12);
        put_utf16(u"ShowMaskOnly");tm.resize(tm.size()+8,0);tm[tm.size()-8]=1;
        Bytes ev=list_chunk(event_type,{chunk(event_meta,em),chunk(take_meta,tm)});
        Bytes event_collection=list_chunk(track_header,{ev});
        return list_chunk(track_type,{th_leaf,event_collection});
    };
    Bytes root(40,0);std::copy(riff.begin(),riff.end(),root.begin());std::copy(form.begin(),form.end(),root.begin()+24);
    Bytes collection=list_chunk(tracks_type,{make_track(2,0.0,49.5161333,0.0),make_track(3,50.2168333,40.8918667,49.5161333)});
    root.insert(root.end(),collection.begin(),collection.end());put64(root,16,root.size());
    const auto tmp_path=std::filesystem::temp_directory_path()/path_from_utf8("koralo vegas event timing test.veg");{std::ofstream f(tmp_path,std::ios::binary);f.write(reinterpret_cast<const char*>(root.data()),static_cast<std::streamsize>(root.size()));}
    std::string error;auto info=inspect_vegas_project(path_to_utf8(tmp_path),&error);req(info.has_value(),"inspect VEG TrackEvent records");
    req(info->tracks.size()==2,"VEG track order recovered");req(info->tracks[0].raw_kind==2&&info->tracks[1].raw_kind==3,"VEG track discriminators recovered");
    req(info->tracks[0].events.size()==1&&info->tracks[1].events.size()==1,"VEG events recovered");
    req(near(info->tracks[0].events[0].start,0.0)&&std::abs(info->tracks[0].events[0].duration-49.5161333)<1e-5,"VEG first event timing recovered");
    req(std::abs(info->tracks[1].events[0].start-50.2168333)<1e-5&&std::abs(info->tracks[1].events[0].duration-40.8918667)<1e-5,"VEG second event timing recovered");
    req(std::abs(info->tracks[1].events[0].source_offset-49.5161333)<1e-5,"VEG Take offset recovered");
    req(info->tracks[0].events[0].video_fx_ids.size()==1&&info->tracks[0].events[0].video_fx_ids[0].find("chromakeyer")!=std::string::npos,"VEG event FX identity recovered");
    const auto& ck=info->tracks[0].events[0].chroma_key;
    req(ck.valid,"VEG Svfx Chroma Key named values recovered");
    req(std::abs(ck.color.r-0.16)<1e-9&&std::abs(ck.color.g-0.95)<1e-9&&std::abs(ck.color.b-0.09)<1e-9,"VEG Chroma Key color recovered");
    req(std::abs(ck.low_threshold-0.07)<1e-9&&std::abs(ck.high_threshold-0.31)<1e-9&&std::abs(ck.blur_amount-0.12)<1e-9&&ck.show_mask_only,"VEG Chroma Key thresholds recovered");
    std::filesystem::remove(tmp_path);
}



static void test_runtime_localization_pack() {
    const auto root=std::filesystem::temp_directory_path()/path_from_utf8("koralo locale test");
    const auto dir=root/"locales";std::filesystem::create_directories(dir);
    {std::ofstream f(dir/"en-US.lang",std::ios::binary);f<<"meta.code=en-US\nmeta.name=English\nmenu.file=File\ncontext.event.copy=Copy\n";}
    {std::ofstream f(dir/"zz-ZZ.lang",std::ios::binary);f<<"meta.code=zz-ZZ\nmeta.name=Test Language\nmenu.file=Fayl\n";}
    Localization loc({path_to_utf8(root)});
    req(loc.languages().size()==2,"localization discovers external packs");
    req(loc.set_language("zz-ZZ"),"localization selects external pack");
    req(loc.text("menu.file","File")=="Fayl","localized value loaded without recompilation");
    req(loc.text("context.event.copy","Copy")=="Copy","missing key falls back to en-US");
    std::filesystem::remove_all(root);
}


static void test_still_image_event_can_extend() {
    Project p=Project::make_default();MediaItem m;m.path="photo.png";m.name="photo";m.duration=5.0;m.has_video=true;const int mid=p.add_media(m).id;int vt=0;for(const auto&t:p.tracks)if(t.kind==TrackKind::Video){vt=t.id;break;}Clip& clip=p.add_clip(mid,vt,0.0,0.0,5.0);
    EditController e;UndoStack undo;TimelineGeometry g=geometry();const int y=g.y+g.ruler_h+20;const int right=e.x_at(g,clip.end());e.begin_clip_drag(p,g,clip.id,right,y,DragMode::TrimRight,false);e.update_drag(p,g,right+700,y,0.0);req(e.drag.preview.contains(clip.id)&&e.drag.preview.at(clip.id).duration>10.0,"still preview can extend beyond nominal source duration");req(e.commit_drag(p,undo,g,right+700,y,0.0),"still trim commit");req(p.clip_by_id(clip.id)->duration>10.0,"still event duration extends freely");
}


static void test_project_preview_settings_roundtrip() {
    Project p=Project::make_default();
    p.settings.preview_quality=3;
    p.settings.preview_fps_limit=60;
    p.settings.use_prepared_cache=false;
    const auto tmp_path=std::filesystem::temp_directory_path()/path_from_utf8("koralo preview settings test.kora");
    req(p.save(path_to_utf8(tmp_path)),"save project preview settings");
    std::string error;auto loaded=Project::load(path_to_utf8(tmp_path),&error);
    req(loaded.has_value(),"load project preview settings");
    req(loaded->settings.preview_quality==3,"preview quality belongs to project");
    req(loaded->settings.preview_fps_limit==60,"preview FPS belongs to project");
    req(!loaded->settings.use_prepared_cache,"prepared-cache policy belongs to project");
    std::filesystem::remove(tmp_path);
}

static void test_generated_text_roundtrip() {
    Project p=Project::make_default();
    MediaItem m;
    m.name="Titles & Text";
    m.duration=7.5;
    m.width=1920;
    m.height=1080;
    m.has_video=true;
    m.video_codec="koralo.generated.titles_text";
    m.generated_text.enabled=true;
    m.generated_text.text="Hello\nKoraLo";
    m.generated_text.font_family="Open Sans";
    m.generated_text.font_size=72.0;
    m.generated_text.bold=true;
    m.generated_text.italic=true;
    m.generated_text.alignment=2;
    m.generated_text.color_r=0.9;
    m.generated_text.color_g=0.7;
    m.generated_text.color_b=0.2;
    m.generated_text.scale=1.35;
    m.generated_text.location_x=0.27;
    m.generated_text.location_y=0.68;
    m.generated_text.anchor=7;
    m.generated_text.outline=true;
    m.generated_text.outline_size=3.0;
    m.generated_text.shadow=true;
    m.generated_text.shadow_x=6.0;
    m.generated_text.shadow_y=5.0;
    m.generated_text.shadow_opacity=0.42;
    m.generated_text.animation=1;
    const int media_id=p.add_media(m).id;
    int vt=0;for(const auto& t:p.tracks)if(t.kind==TrackKind::Video){vt=t.id;break;}
    p.add_clip(media_id,vt,1.0,0.0,7.5);
    const auto tmp_path=std::filesystem::temp_directory_path()/path_from_utf8("koralo generated text roundtrip.kora");
    req(p.save(path_to_utf8(tmp_path)),"save generated text project");
    std::string error;auto loaded=Project::load(path_to_utf8(tmp_path),&error);req(loaded.has_value(),"load generated text project");
    req(loaded->media.size()==1,"generated text media roundtrip");
    const auto& g=loaded->media.front().generated_text;
    req(g.enabled&&g.text=="Hello\nKoraLo","generated text content roundtrip");
    req(g.font_family=="Open Sans"&&near(g.font_size,72.0)&&g.bold&&g.italic,"generated text font roundtrip");
    req(g.alignment==2&&near(g.location_x,0.27)&&near(g.location_y,0.68)&&g.anchor==7,"generated text layout roundtrip");
    req(g.outline&&near(g.outline_size,3.0)&&g.shadow&&near(g.shadow_opacity,0.42)&&g.animation==1,"generated text styling roundtrip");
    std::filesystem::remove(tmp_path);
}

static void test_native_audio_fx_roundtrip() {
    const auto defs=available_audio_effect_definitions();
    req(defs.size()>=12,"native audio FX catalog populated");
    req(audio_effect_definition("koralo.audio.echo")!=nullptr,"echo definition exists");
    req(audio_effect_definition("koralo.audio.reverb")!=nullptr,"reverb definition exists");
    Project p=Project::make_default();
    MediaItem m;m.path="voice.wav";m.name="voice";m.duration=3.0;m.has_audio=true;m.audio_channels=1;m.sample_rate=48000;
    const int mid=p.add_media(m).id;int at=0;for(const auto& t:p.tracks)if(t.kind==TrackKind::Audio){at=t.id;break;}
    Clip& clip=p.add_clip(mid,at,0.0,0.0,3.0);const int clip_id=clip.id;auto echo=make_audio_effect_instance("koralo.audio.echo",p.next_id());set_effect_numeric(echo,"delay_ms",420.0);clip.audio_effects.push_back(echo);
    Track* track=p.track_by_id(at);auto comp=make_audio_effect_instance("koralo.audio.compressor",p.next_id());set_effect_numeric(comp,"ratio",5.5);track->audio_effects.push_back(comp);
    UndoStack undo;undo.begin(p);set_effect_numeric(clip.audio_effects.front(),"wet",0.7);req(undo.commit(p),"audio FX parameter enters undo");req(undo.undo(p),"audio FX parameter undo");
    // Undo replaces the whole Project snapshot, invalidating references into its
    // vectors.  Keep stable IDs across the transaction instead of reading the
    // now-dangling `clip` reference. GCC happened to tolerate this in earlier
    // builds; MSVC exposed the undefined behavior as a roundtrip test failure.
    const auto tmp=std::filesystem::temp_directory_path()/path_from_utf8("koralo audio fx roundtrip.kora");req(p.save(path_to_utf8(tmp)),"save native audio FX");std::string error;auto loaded=Project::load(path_to_utf8(tmp),&error);req(loaded.has_value(),"load native audio FX");
    const Clip* lc=loaded->clip_by_id(clip_id);const Track* lt=loaded->track_by_id(at);req(lc&&lc->audio_effects.size()==1,"event audio FX roundtrip");req(lt&&lt->audio_effects.size()==1,"track audio FX roundtrip");req(near(effect_numeric(lc->audio_effects.front(),"delay_ms",0.0,0.0),420.0),"audio FX parameter roundtrip");
    std::filesystem::remove(tmp);
}

static void test_imported_audio_fx_roundtrip() {
    Project p=Project::make_default();
    Track* audio=nullptr;for(auto& track:p.tracks)if(track.kind==TrackKind::Audio){audio=&track;break;}
    req(audio!=nullptr,"default project has audio track");
    audio->imported_audio_fx={"Sony Track Noise Gate","Sony Track EQ","Sony Track Compressor"};
    const auto tmp_path=std::filesystem::temp_directory_path()/path_from_utf8("koralo imported audio fx refs.kora");
    req(p.save(path_to_utf8(tmp_path)),"save imported audio FX references");
    std::string error;auto loaded=Project::load(path_to_utf8(tmp_path),&error);req(loaded.has_value(),"load imported audio FX references");
    const Track* loaded_audio=nullptr;for(const auto& track:loaded->tracks)if(track.kind==TrackKind::Audio){loaded_audio=&track;break;}
    req(loaded_audio&&loaded_audio->imported_audio_fx.size()==3,"opaque imported audio FX chain survives .kora roundtrip");
    req(loaded_audio->imported_audio_fx[1]=="Sony Track EQ","imported audio FX order survives roundtrip");
    std::filesystem::remove(tmp_path);
}

static void test_timeline_12_hour_limit() {
    Project p=Project::make_default();
    MediaItem m; m.path="long.mp4"; m.name="long"; m.duration=kTimelineMaxSeconds+100.0; m.has_video=true;
    const int mid=p.add_media(m).id; int vt=0; for(const auto& t:p.tracks) if(t.kind==TrackKind::Video){vt=t.id;break;}
    Clip& c=p.add_clip(mid,vt,kTimelineMaxSeconds-1.0,0.0,30.0);
    req(c.end()<=kTimelineMaxSeconds+1e-9,"clip constrained to 12-hour timeline");
    EditController e; TimelineGeometry g=geometry(); g.scroll_sec=kTimelineMaxSeconds-2.0;
    req(e.time_at(g,1000000)<=kTimelineMaxSeconds,"time mapping clamps at 12 hours");
    UndoStack undo; e.selection.select_clip(p,c.id,SelectionOperation::Replace,false);
    req(!e.nudge_selected(p,undo,10.0),"nudge beyond 12-hour edge produces no change");
    req(c.end()<=kTimelineMaxSeconds+1e-9,"nudge cannot cross 12-hour edge");
}

static void test_ofx_discovery_and_effect_cost() {
#ifdef KORALO_TEST_OFX_ROOT
#ifdef _WIN32
    _putenv_s("OFX_PLUGIN_PATH", KORALO_TEST_OFX_ROOT);
#else
    setenv("OFX_PLUGIN_PATH", KORALO_TEST_OFX_ROOT, 1);
#endif
    OfxHostRegistry registry; registry.scan_paths({KORALO_TEST_OFX_ROOT});
    req(registry.image_effect_count()>=1,"OFX scanner discovers image-effect binary");
    req(registry.boris_fx_count()>=1,"OFX scanner identifies Boris-style plug-in id");
    bool found=false;for(const auto& p:registry.plugins())if(p.identifier=="com.borisfx.KoraLoFakeContinuum"&&p.image_effect&&p.binary_loaded)found=true;
    req(found,"OFX scanner reads exported plug-in identity");
    req(registry.last_error().empty(),"healthy OFX scan has no scanner error");

    const std::vector<std::string> protocol_paths{"plain", "quote\"and\\slash", "тест/плагин"};
    std::vector<std::string> decoded_paths;std::string protocol_error;
    req(parse_ofx_scan_request(make_ofx_scan_request(protocol_paths),decoded_paths,&protocol_error),"OFX JSONL request roundtrip parses");
    req(decoded_paths==protocol_paths,"OFX JSONL request preserves UTF-8 and escaping");
#endif
#ifdef KORALO_TEST_BAD_OFX_ROOT
    OfxHostRegistry bad_registry; bad_registry.scan_paths({KORALO_TEST_BAD_OFX_ROOT});
    req(bad_registry.plugins().empty(),"crashing OFX plug-in cannot leak partial metadata into editor");
    req(!bad_registry.last_error().empty(),"crashing OFX plug-in is reported as isolated scanner failure");
#endif
    std::vector<EffectInstance> light{make_effect_instance("koralo.brightness_contrast",1)};
    std::vector<EffectInstance> heavy{make_effect_instance("koralo.anime_line_restore",2),make_effect_instance("koralo.gaussian_blur",3)};
    req(effect_preview_cost(light)<effect_preview_cost(heavy),"heavy FX chain has larger realtime preview cost");
}

int main() {
    try {
        test_project_and_undo();
        test_project_semantic_roundtrip();
        test_project_load_rejects_duplicate_settings_and_generator_conflicts();
        test_ofx_discovery_and_effect_cost();
        test_group_aware_selection();
        test_detach_linked_audio_group();
        test_ignore_event_grouping_selection();
        test_take_model_and_roundtrip();
        test_v5_project_backward_load();
        test_video_motion_keyframes_and_split();
        test_video_motion_project_roundtrip_and_undo();
        test_effect_model_roundtrip_and_undo();
        test_vegas_riff_inspection();
        test_vegas12_settings_and_wmv_inspection();
        test_vegas_event_timing_inspection();
        test_timeline_vertical_scroll_hit_testing();
        test_timeline_vertical_scrollbar_geometry();
        test_timeline_horizontal_scroll_keeps_header_noninteractive();
        test_timeline_12_hour_limit();
        test_still_image_event_can_extend();
        test_project_preview_settings_roundtrip();
        test_generated_text_roundtrip();
        test_imported_audio_fx_roundtrip();
        test_native_audio_fx_roundtrip();
        test_runtime_localization_pack();
        test_fade_handle_transaction();
        test_group_aware_split_creates_right_group();
        test_split_without_selection();
        test_slip_active_and_all_takes();
        test_take_cycle_and_loop_undo();
        test_quantize_to_frames_move();
        test_trim_start_end_to_cursor();
        test_group_vertical_move_preserves_kind_offset();
        test_marquee_group_selection();
        test_transactional_move_preview();
        test_group_aware_trim_transaction();
        test_multi_selection_move_transaction();
        test_roll_transaction();
        test_slide_transaction();
        test_ripple_trim_transaction();
        test_track_reorder_transaction();
        test_clipboard_group_transaction();
        test_event_lock_blocks_edit();
        test_trimmer_insert_overwrite_transactions();
        test_control_state_undo();
        std::cout << "KoraLo native core tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "KoraLo native core test failure: " << e.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "KoraLo native core test failure: unknown exception\n";
        return 2;
    }
}
