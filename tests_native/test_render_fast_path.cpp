#include "media/render_fast_path.hpp"

#include <stdexcept>
#include <string>
#include <iostream>

using namespace koralo;
static void require(bool ok,const char* msg){if(!ok)throw std::runtime_error(msg);}

int main(){try{
  MediaItem media;
  media.id=1;media.path="clip.mp4";media.width=1920;media.height=1080;
  media.fps=30;media.duration=30;media.video_codec="h264";media.has_video=true;media.has_audio=true;
  Track video;video.id=1;video.kind=TrackKind::Video;
  Track audio;audio.id=2;audio.kind=TrackKind::Audio;
  Clip v;v.id=1;v.track_id=1;v.duration=10;v.takes.push_back({1,0,""});
  Clip a=v;a.id=2;a.track_id=2;
  RenderPlan plan;plan.width=1920;plan.height=1080;plan.fps=30;plan.duration=10;
  plan.inputs={{&v,&video,&media}};
  RenderGraph graph;graph.video_layers.push_back({&v,&video,&media,1.0,0.0,0.0,{}});
  Project project;
  auto pick=[&]{return choose_direct_export(project,plan,graph);};
  require(pick() && !pick()->include_audio,"single video direct export");
  graph.audio_layers.push_back({&a,&audio,&media,1.0,0.0,0.0,0.0,{}});
  plan.inputs.push_back({&a,&audio,&media});
  require(pick() && pick()->include_audio,"paired audio direct export");
  a.fade_in=0.25;graph.audio_layers[0].fade_in=.25;
  require(!pick(),"audio fade requires mix graph");
  a.fade_in=0;graph.audio_layers[0].fade_in=0;
  audio.pan=.4;graph.audio_layers[0].pan=.4;
  require(!pick(),"audio pan requires mix graph");
  audio.pan=0;graph.audio_layers[0].pan=0;
  require(pick().has_value(),"restored audio");
  media.duration=2;require(!pick(),"short media must preserve timeline background");media.duration=30;
  v.video_motion.enabled=true;require(!pick(),"motion requires graph");v.video_motion.enabled=false;
  v.start=.1;require(!pick(),"timeline gap requires graph");v.start=0;
  v.rate=1.2;require(!pick(),"speed requires graph");v.rate=1;
  v.opacity=.5;graph.video_layers[0].opacity=.5;require(!pick(),"opacity requires graph");
  v.opacity=1;graph.video_layers[0].opacity=1;
  v.fade_out=.2;graph.video_layers[0].fade_out=.2;require(!pick(),"fades require graph");
  v.fade_out=0;graph.video_layers[0].fade_out=0;
  media.video_codec="vp9";require(!pick(),"alpha-capable codec must be composited");media.video_codec="h264";
  media.generated_text.enabled=true;require(!pick(),"generated media cannot be direct");media.generated_text.enabled=false;
  project.video_output_effects.push_back(EffectInstance{1,"koralo.blur",false,{}});
  require(pick().has_value(),"disabled output effects do not force graph");
  project.video_output_effects.back().enabled=true;
  require(!pick(),"enabled output effects require graph");
  project.video_output_effects.clear();
  v.video_effects.push_back(EffectInstance{2,"koralo.blur",false,{}});
  require(pick().has_value(),"disabled clip FX permit direct export");
  v.video_effects.back().enabled=true;require(!pick(),"active clip FX require graph");
  v.video_effects.clear();
  plan.native_prepared=true;require(!pick(),"native-prepared stages keep established path");plan.native_prepared=false;
  plan.inputs.push_back({&v,&video,&media});require(!pick(),"extra input requires graph");plan.inputs.pop_back();
  require(pick().has_value(),"simple source remains direct");
  {
    Clip left=v, right=v;
    left.id=11;left.duration=4.0;left.start=0;
    right.id=12;right.duration=6.0;right.start=4.0;right.set_source_offset(4.0);
    RenderGraph cuts;
    cuts.video_layers={{&left,&video,&media,1.0,0,0,{}},
                       {&right,&video,&media,1.0,0,0,{}}};
    RenderPlan cut_plan=plan;
    cut_plan.inputs={{&left,&video,&media},{&right,&video,&media}};
    auto cuts_route=[&]{return choose_cut_export(project,cut_plan,cuts);};
    require(cuts_route()&&cuts_route()->size()==2,"two sequential cuts use fast path");
    require((*cuts_route())[0].frames==120 && (*cuts_route())[1].frames==180,
            "frame accurate cut lengths");
    right.start=4.1;require(!cuts_route(),"gaps require compositor");right.start=3.9;
    require(!cuts_route(),"overlaps require compositor");right.start=4.0;
    right.video_motion.enabled=true;require(!cuts_route(),"motion requires compositor");right.video_motion.enabled=false;
    right.fade_in=.25;cuts.video_layers[1].fade_in=.25;
    require(!cuts_route(),"fade requires compositor");right.fade_in=0;cuts.video_layers[1].fade_in=0;
    right.opacity=.7;cuts.video_layers[1].opacity=.7;
    require(!cuts_route(),"opacity requires compositor");right.opacity=1;cuts.video_layers[1].opacity=1;
    right.rate=1.25;require(!cuts_route(),"rate changes require compositor");right.rate=1;
    right.duration=5.9;require(!cuts_route(),"partial timeline requires compositor");right.duration=6;
    right.duration=5.975;require(!cuts_route(),"sub-frame cuts require compositor");right.duration=6;
    right.track_id=2;cuts.video_layers[1].track=&audio;
    require(!cuts_route(),"multiple tracks require compositor");
    right.track_id=1;cuts.video_layers[1].track=&video;
    right.video_effects.push_back(EffectInstance{3,"koralo.blur",true,{}});
    require(!cuts_route(),"effects require compositor");right.video_effects.clear();
    cuts.audio_layers.push_back({&a,&audio,&media,1.0,0,0,0,{}});
    cut_plan.inputs.push_back({&a,&audio,&media});
    require(cuts_route().has_value(),"independent audio remains in standard audio graph");
  }
  std::cout << "Direct and fast-cut export route tests passed\n";
  return 0;
}catch(const std::exception& ex){std::cerr<<ex.what()<<'\n';return 1;}}
