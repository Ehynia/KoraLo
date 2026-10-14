#include "media/render_graph.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace koralo;

static void req(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static bool near(double a,double b){return std::abs(a-b)<1e-9;}

int main(){
    try{
        Project p=Project::make_default();
        int vt=0,at=0;for(auto& t:p.tracks){if(!vt&&t.kind==TrackKind::Video)vt=t.id;if(!at&&t.kind==TrackKind::Audio)at=t.id;}
        MediaItem vm;vm.name="video";vm.path="video.mp4";vm.duration=10;vm.has_video=true;const int vmid=p.add_media(vm).id;
        MediaItem am;am.name="audio";am.path="audio.wav";am.duration=10;am.has_audio=true;const int amid=p.add_media(am).id;
        Clip& v=p.add_clip(vmid,vt,1.0,0.0,4.0);v.opacity=.5;v.fade_in=.25;v.fade_out=.5;
        Clip& a=p.add_clip(amid,at,0.5,0.0,3.0);a.gain=.8;a.fade_in=.2;a.fade_out=.3;
        auto* vtrack=p.track_by_id(vt);auto* atrack=p.track_by_id(at);req(vtrack&&atrack,"tracks");vtrack->opacity=.6;atrack->gain=.5;atrack->pan=-.25;
        p.video_output_effects.push_back(make_effect_instance("koralo.brightness_contrast",p.next_id()));
        const auto out=std::filesystem::temp_directory_path()/"koralo-render-graph-test.mp4";
        std::string error;auto plan=plan_render_project(p,out.string(),nullptr,false,nullptr,&error);req(plan.has_value(),"plan");
        auto graph=build_render_graph(p,*plan);
        req(graph.video_layers.size()==1&&graph.audio_layers.size()==1,"graph layer counts");
        const auto& vl=graph.video_layers.front();req(near(vl.opacity,.3)&&near(vl.fade_in,.25)&&near(vl.fade_out,.5),"video graph values");
        req(vl.stages.size()==7&&vl.stages.front()==RenderStageKind::Source&&vl.stages.back()==RenderStageKind::Blend,"video stage order");
        const auto& al=graph.audio_layers.front();req(near(al.gain,.4)&&near(al.pan,-.25)&&near(al.fade_in,.2)&&near(al.fade_out,.3),"audio graph values");
        req(al.stages.size()==6&&al.stages.front()==RenderStageKind::Source&&al.stages.back()==RenderStageKind::AudioMix,"audio stage order");
        req(graph.has_output_effects&&graph.output_stages.size()==1&&graph.output_stages.front()==RenderStageKind::OutputEffects,"output stage");
        std::cout<<"KoraLo render graph tests passed\n";
        return 0;
    }catch(const std::exception& ex){std::cerr<<ex.what()<<'\n';return 1;}
}
