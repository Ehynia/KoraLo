#include "ui/editor_app.hpp"
#include "platform/app_paths.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
using namespace koralo;
namespace fs=std::filesystem;
void require(bool b,const char* s){if(!b)throw std::runtime_error(s);}
struct Services:PlatformServices{
    std::string project,output,last_error;int redraws=0;
    std::vector<std::string> open_media_files()override{return {};}
    std::string open_project_file()override{return project;}
    std::string save_project_file(const std::string&)override{return {};}
    std::string save_render_file(const std::string&)override{return output;}
    void show_message(const std::string&,const std::string& text,bool error=false)override{if(error)last_error=text;}
    SaveChangesChoice ask_save_changes(const std::string&)override{return SaveChangesChoice::Discard;}
    void set_title(const std::string&)override{}
    void request_redraw()override{++redraws;}
    std::string executable_dir()const override{return path_to_utf8(fs::path(__FILE__).parent_path().parent_path());}
};
std::string xml(std::string s){std::string o;for(char c:s){if(c=='&')o+="&amp;";else if(c=='<')o+="&lt;";else if(c=='>')o+="&gt;";else if(c=='\"')o+="&quot;";else o+=c;}return o;}
struct RecordingCanvas:Canvas{
    struct Label{int x,y;std::string value;};std::vector<Label> labels;std::ostringstream svg;int clip=0,depth=0;
    int width()const override{return 1280;}int height()const override{return 800;}
    std::string color(Color c){return "rgb("+std::to_string(c.r)+","+std::to_string(c.g)+","+std::to_string(c.b)+")";}
    void reset(){labels.clear();svg.str({});svg.clear();clip=depth=0;svg<<"<svg xmlns='http://www.w3.org/2000/svg' width='1280' height='800'>";}
    void fill_rect(Rect r,Color c)override{svg<<"<rect x='"<<r.x<<"' y='"<<r.y<<"' width='"<<std::max(0,r.w)<<"' height='"<<std::max(0,r.h)<<"' fill='"<<color(c)<<"' opacity='"<<c.a/255.0<<"'/>";}
    void stroke_rect(Rect r,Color c,int thickness=1)override{svg<<"<rect x='"<<r.x<<"' y='"<<r.y<<"' width='"<<std::max(0,r.w)<<"' height='"<<std::max(0,r.h)<<"' fill='none' stroke='"<<color(c)<<"' stroke-width='"<<thickness<<"'/>";}
    void line(int a,int b,int c,int d,Color col,int thickness=1)override{svg<<"<path d='M"<<a<<","<<b<<" L"<<c<<","<<d<<"' stroke='"<<color(col)<<"' stroke-width='"<<thickness<<"'/>";}
    int text_width(const std::string& s,int size=12,bool bold=false)override{int n=0;for(unsigned char c:s)if((c&0xc0)!=0x80)++n;return int(n*size*(bold?.63:.59));}
    void text(int x,int y,const std::string& s,Color c,int size=12,bool bold=false)override{labels.push_back({x,y,s});svg<<"<text x='"<<x<<"' y='"<<y<<"' font-family='sans-serif' font-size='"<<size<<"' font-weight='"<<(bold?700:400)<<"' fill='"<<color(c)<<"'>"<<xml(s)<<"</text>";}
    void image(Rect,const Frame&)override{}
    void push_clip(Rect r)override{const int id=++clip;++depth;svg<<"<defs><clipPath id='clip"<<id<<"'><rect x='"<<r.x<<"' y='"<<r.y<<"' width='"<<std::max(0,r.w)<<"' height='"<<std::max(0,r.h)<<"'/></clipPath></defs><g clip-path='url(#clip"<<id<<")'>";}
    void pop_clip()override{svg<<"</g>";--depth;}
    bool has(const std::string& text)const{return std::any_of(labels.begin(),labels.end(),[&](const auto& l){return l.value==text;});}
    void save(const std::string& name){if(const auto* dir=std::getenv("KORALO_UI_QA_DIR")){fs::create_directories(path_from_utf8(dir));std::ofstream(path_from_utf8(dir)/(name+".svg"))<<svg.str()<<"</svg>";}}
};
void draw(EditorApp& app,RecordingCanvas& c){c.reset();app.draw(c);require(c.depth==0,"canvas clip stack balanced");}
void click(EditorApp& app,RecordingCanvas& c,const std::string& name){draw(app,c);for(auto it=c.labels.rbegin();it!=c.labels.rend();++it)if(const auto& l=*it;l.value==name){app.mouse_down(l.x+2,l.y-5,1,false,false,false);app.mouse_up(l.x+2,l.y-5,1,false,false,false);return;}throw std::runtime_error("Missing control: "+name);}
int main(int argc,char** argv){try{
    (void)argc;initialize_executable_path(argv[0]);
    MediaEngine engine;if(!engine.ready())return 77;
    TemporaryFile input(temporary_directory(),".kora"),output(temporary_directory(),".mp4");
    Project project;project.settings.width=640;project.settings.height=360;project.settings.fps=12;
    MediaItem title;title.has_video=true;title.duration=.5;title.generated_text.enabled=true;title.generated_text.text="Export test";
    const int id=project.add_media(title).id,track=project.add_track(TrackKind::Video).id;project.add_clip(id,track,0,0,.5);
    require(project.save(path_to_utf8(input.path())),"save UI fixture");
    Services services;services.project=path_to_utf8(input.path());services.output=path_to_utf8(output.path());
    Localization locale({services.executable_dir()});const char* lang=std::getenv("KORALO_LANG");locale.set_language(lang?lang:"en-US");
    auto tr=[&](const char* key,const char* fallback){return locale.text(key,fallback);};
    EditorApp app(services);app.set_ui_scale(1);RecordingCanvas canvas;
    app.key_down(Key::O,true,false,false);
    click(app,canvas,tr("menu.file","File"));click(app,canvas,tr("menu.file.render","Render As..."));
    require(canvas.has(tr("menu.file.render","Render As...")),"export menu reachable");
    draw(app,canvas);require(canvas.has(tr("render.title","Export video")),"new export dialog visible");canvas.save("r45-export-settings");
    click(app,canvas,"720p");click(app,canvas,"[ ] FSR 1 (EASU + RCAS)");
    draw(app,canvas);require(canvas.has("[x] FSR 1 (EASU + RCAS)"),"FSR control toggles");
    click(app,canvas,tr("render.start","Render"));
    draw(app,canvas);canvas.save("r45-export-progress");if(!services.last_error.empty())throw std::runtime_error(services.last_error);require(canvas.has(tr("render.job","Export job")),"progress view remains open after start");
    const int redraws=services.redraws;bool finished=false;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(90);
    while(std::chrono::steady_clock::now()<deadline){app.tick();draw(app,canvas);if(canvas.has(tr("render.stage.fsr","FSR 1 upscaling")))canvas.save("r45-export-progress");if(canvas.has(tr("status.render_complete","Export complete"))){finished=true;break;}if(!services.last_error.empty())throw std::runtime_error(services.last_error);std::this_thread::sleep_for(std::chrono::milliseconds(30));}
    require(finished&&canvas.has("100.0%"),"export completion and final percent visible");
    require(services.redraws>redraws,"export progress schedules repaint without playback");canvas.save("r45-export-complete");
    click(app,canvas,tr("common.close","Close"));draw(app,canvas);require(!canvas.has(tr("render.job","Export job")),"completed dialog closes");
    const auto completed_size=fs::file_size(output.path());
    click(app,canvas,tr("menu.file","File"));click(app,canvas,tr("menu.file.render","Render As..."));
    click(app,canvas,tr("render.start","Render"));click(app,canvas,tr("render.background","Continue in background"));
    click(app,canvas,tr("menu.file","File"));click(app,canvas,tr("menu.file.render","Render As..."));
    draw(app,canvas);require(canvas.has(tr("render.job","Export job")),"background task can be reopened");
    click(app,canvas,tr("render.stop","Cancel export"));
    bool stopped=false;const auto stop_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    while(std::chrono::steady_clock::now()<stop_deadline){app.tick();draw(app,canvas);if(canvas.has(tr("status.render_cancelled","Export cancelled"))){stopped=true;break;}std::this_thread::sleep_for(std::chrono::milliseconds(20));}
    require(stopped,"cancel button stops active export");require(fs::file_size(output.path())==completed_size,"UI cancel preserves previously completed file");
    std::cout<<"PASS: export settings, FSR toggle, live progress, completion, background/reopen and cancellation\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
