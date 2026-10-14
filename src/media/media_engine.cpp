#include "fsr_upscaler.hpp"
#include "fsr_cuda.hpp"
#include "native_cuda.hpp"
#include "export_parallelism.hpp"
#include "render_control.hpp"
#include <future>
#include "media_engine.hpp"
#include "audio_preview_timing.hpp"
#include "audio_mix_graph.hpp"
#include "render_graph.hpp"
#include "render_fast_path.hpp"
#include "media_probe.hpp"
#include "vector_renderer.hpp"
#include "native_effects.hpp"
#include <memory>
#include "../platform/process.hpp"
#include "../ui/inter_atlas.hpp"
#include "../ui/open_sans_atlas.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include "../platform/app_paths.hpp"
#include <future>
#include <iomanip>
#include <sstream>

namespace koralo {
namespace fs = std::filesystem;

namespace {
bool read_ppm(const std::string& path, Frame& f){
    std::ifstream in(path_from_utf8(path),std::ios::binary);if(!in)return false;std::string magic;in>>magic;if(magic!="P6")return false;
    int maxv=0;in>>f.width>>f.height>>maxv;in.get();if(f.width<=0||f.height<=0||maxv!=255)return false;
    f.rgb.resize(static_cast<std::size_t>(f.width)*f.height*3);in.read(reinterpret_cast<char*>(f.rgb.data()),static_cast<std::streamsize>(f.rgb.size()));return in.good()||in.gcount()==static_cast<std::streamsize>(f.rgb.size());
}

std::string temp_name(const std::string& suffix){TemporaryFile file(temporary_directory(),suffix);return file.valid()?path_to_utf8(file.release()):std::string{};}

std::string number_expr(double value){
    std::ostringstream s;s<<std::fixed<<std::setprecision(8)<<value;return s.str();
}

std::string animated_expr(const AnimatedDouble& animated,const std::string& time_expr){
    if(animated.keys.empty())return number_expr(animated.default_value);
    if(animated.keys.size()==1)return number_expr(animated.keys.front().value);
    std::string expr=number_expr(animated.keys.back().value);
    for(std::size_t i=animated.keys.size()-1;i>0;--i){
        const auto&a=animated.keys[i-1];const auto&b=animated.keys[i];
        std::string segment;
        if(a.interpolation==KeyframeInterpolation::Hold)segment=number_expr(a.value);
        else segment="("+number_expr(a.value)+"+("+number_expr(b.value-a.value)+")*("+time_expr+"-"+number_expr(a.time)+")/"+number_expr(std::max(1e-9,b.time-a.time))+")";
        expr="if(lt("+time_expr+","+number_expr(b.time)+"),"+segment+","+expr+")";
    }
    const auto&first=animated.keys.front();
    return "if(lt("+time_expr+","+number_expr(first.time)+"),"+number_expr(first.value)+","+expr+")";
}

std::string effect_param_expr(const EffectParameterState& param,const std::string& time_expr,double fallback){
    if(!param.animated||param.keys.empty())return number_expr(param.numeric_value(0.0,fallback));
    if(param.keys.size()==1)return number_expr(param.keys.front().value);
    std::string expr=number_expr(param.keys.back().value);
    for(std::size_t i=param.keys.size()-1;i>0;--i){
        const auto&a=param.keys[i-1];const auto&b=param.keys[i];
        std::string segment=a.interpolation==EffectKeyInterpolation::Hold?number_expr(a.value):
            "("+number_expr(a.value)+"+("+number_expr(b.value-a.value)+")*("+time_expr+"-"+number_expr(a.time)+")/"+number_expr(std::max(1e-9,b.time-a.time))+")";
        expr="if(lt("+time_expr+","+number_expr(b.time)+"),"+segment+","+expr+")";
    }
    return "if(lt("+time_expr+","+number_expr(param.keys.front().time)+"),"+number_expr(param.keys.front().value)+","+expr+")";
}

std::uint8_t clamp_byte(double value){
    return static_cast<std::uint8_t>(std::clamp(std::lround(value),0l,255l));
}

std::vector<std::uint32_t> utf8_codepoints(const std::string& text){
    std::vector<std::uint32_t> out;
    for(std::size_t i=0;i<text.size();){
        const unsigned char c=static_cast<unsigned char>(text[i]);
        if(c<0x80){out.push_back(c);++i;continue;}
        std::uint32_t cp=0;int count=0;
        if((c&0xE0)==0xC0){cp=c&0x1F;count=2;}
        else if((c&0xF0)==0xE0){cp=c&0x0F;count=3;}
        else if((c&0xF8)==0xF0){cp=c&0x07;count=4;}
        else{out.push_back('?');++i;continue;}
        if(i+static_cast<std::size_t>(count)>text.size()){out.push_back('?');break;}
        bool ok=true;
        for(int k=1;k<count;++k){const unsigned char cc=static_cast<unsigned char>(text[i+k]);if((cc&0xC0)!=0x80){ok=false;break;}cp=(cp<<6)|(cc&0x3F);}
        out.push_back(ok?cp:static_cast<std::uint32_t>('?'));i+=ok?count:1;
    }
    return out;
}

struct TextRaster { int width=0,height=0; std::vector<std::uint8_t> rgba; };

TextRaster rasterize_generated_text(const GeneratedText& text,int width,int height){
    TextRaster out;out.width=std::max(1,width);out.height=std::max(1,height);out.rgba.assign(static_cast<std::size_t>(out.width)*out.height*4u,0);
    if(!text.enabled||text.text.empty())return out;
    const double desired=std::clamp(text.font_size*text.scale,7.0,320.0);
    const int atlas_size=std::clamp(static_cast<int>(std::lround(std::min(28.0,desired))),7,28);
    const double glyph_scale=desired/atlas_size;
    // Existing projects retain their title typeface; UI and new titles use
    // Inter. Changing the application font must not silently restyle edits.
    const bool legacy_font=text.font_family=="Open Sans";
    const auto* font_pixels=legacy_font?open_sans_atlas::kPixels:inter_atlas::kPixels;
    const double coverage_max=legacy_font?3.0:15.0;
    auto font_glyph=[&](std::uint32_t cp){return legacy_font?open_sans_atlas::find(cp,atlas_size,text.bold,text.italic):inter_atlas::find(cp,atlas_size,text.bold,text.italic);};
    const int line_height=std::max(8,static_cast<int>(std::lround(desired*1.24)));
    std::vector<std::string> lines;std::stringstream ss(text.text);std::string line;
    while(std::getline(ss,line)) lines.push_back(line);
    if(lines.empty()) lines.emplace_back();
    auto line_width=[&](const std::string& value){double w=0;for(auto cp:utf8_codepoints(value)){if(cp=='\r')continue;const auto*g=font_glyph(cp);if(!g)g=font_glyph('?');if(g)w+=g->advance*glyph_scale;}return static_cast<int>(std::lround(w));};
    std::vector<int> widths;int box_w=1;for(const auto& l:lines){int lw=line_width(l);widths.push_back(lw);box_w=std::max(box_w,lw);}const int box_h=std::max(line_height,static_cast<int>(lines.size())*line_height);
    const int ax=std::clamp(text.anchor%3,0,2),ay=std::clamp(text.anchor/3,0,2);
    const int anchor_x=static_cast<int>(std::lround(std::clamp(text.location_x,0.0,1.0)*(out.width-1)));
    const int anchor_y=static_cast<int>(std::lround(std::clamp(text.location_y,0.0,1.0)*(out.height-1)));
    const int box_x=anchor_x-(ax==1?box_w/2:(ax==2?box_w:0));
    const int box_y=anchor_y-(ay==1?box_h/2:(ay==2?box_h:0));
    auto blend=[&](int x,int y,double r,double g,double b,double a){if(x<0||y<0||x>=out.width||y>=out.height||a<=0)return;auto i=(static_cast<std::size_t>(y)*out.width+x)*4u;const double da=out.rgba[i+3]/255.0;const double oa=a+da*(1.0-a);if(oa<=1e-9)return;auto comp=[&](int ch,double src){const double dst=out.rgba[i+ch]/255.0;out.rgba[i+ch]=clamp_byte(((src*a+dst*da*(1.0-a))/oa)*255.0);};comp(0,r);comp(1,g);comp(2,b);out.rgba[i+3]=clamp_byte(oa*255.0);};
    auto draw_pass=[&](double offx,double offy,double r,double g,double b,double alpha,int outline_radius){
        for(std::size_t li=0;li<lines.size();++li){
            int start_x=box_x;if(text.alignment==1)start_x+=std::max(0,(box_w-widths[li])/2);else if(text.alignment==2)start_x+=std::max(0,box_w-widths[li]);
            double pen_x=start_x;const int baseline=box_y+static_cast<int>(li)*line_height+static_cast<int>(std::lround(desired));
            for(auto cp:utf8_codepoints(lines[li])){
                const auto*gptr=font_glyph(cp);if(!gptr)gptr=font_glyph('?');if(!gptr)continue;const auto& gph=*gptr;
                for(int gy=0;gy<gph.height;++gy)for(int gx=0;gx<gph.width;++gx){const std::uint8_t cov=font_pixels[gph.offset+static_cast<std::uint32_t>(gy*gph.width+gx)];if(cov==0)continue;const double coverage=std::min(1.0,cov/coverage_max);const int sx0=static_cast<int>(std::floor((gph.left+gx)*glyph_scale));const int sx1=static_cast<int>(std::ceil((gph.left+gx+1)*glyph_scale));const int sy0=static_cast<int>(std::floor((gph.top+gy)*glyph_scale));const int sy1=static_cast<int>(std::ceil((gph.top+gy+1)*glyph_scale));for(int yy=sy0;yy<sy1;++yy)for(int xx=sx0;xx<sx1;++xx){const int italic_shift=0;for(int oy=-outline_radius;oy<=outline_radius;++oy)for(int ox=-outline_radius;ox<=outline_radius;++ox){if(outline_radius>0&&ox*ox+oy*oy>outline_radius*outline_radius)continue;blend(static_cast<int>(std::lround(pen_x+offx))+xx+italic_shift+ox,static_cast<int>(std::lround(baseline+offy))+yy+oy,r,g,b,alpha*coverage);}}}
                pen_x+=gph.advance*glyph_scale;
            }
        }
    };
    if(text.shadow)draw_pass(text.shadow_x,text.shadow_y,0.0,0.0,0.0,std::clamp(text.shadow_opacity,0.0,1.0),0);
    if(text.outline)draw_pass(0,0,std::clamp(text.outline_r,0.0,1.0),std::clamp(text.outline_g,0.0,1.0),std::clamp(text.outline_b,0.0,1.0),std::clamp(text.outline_a,0.0,1.0),std::max(1,static_cast<int>(std::lround(text.outline_size))));
    draw_pass(0,0,std::clamp(text.color_r,0.0,1.0),std::clamp(text.color_g,0.0,1.0),std::clamp(text.color_b,0.0,1.0),std::clamp(text.color_a,0.0,1.0),0);
    return out;
}

void composite_generated_text(Frame& frame,const GeneratedText& text,double opacity,double event_time,double event_duration){
    if(!text.enabled) return;
    if(frame.width<=0||frame.height<=0) return;
    double a=std::clamp(opacity,0.0,1.0);
    if(text.animation==1){
        const double edge=std::min(0.40,std::max(0.08,event_duration*0.12));
        a*=std::min(std::clamp(event_time/edge,0.0,1.0),std::clamp((event_duration-event_time)/edge,0.0,1.0));
    }
    const auto raster=rasterize_generated_text(text,frame.width,frame.height);
    for(std::size_t pixel=0;pixel<static_cast<std::size_t>(frame.width)*frame.height;++pixel){
        const double alpha=raster.rgba[pixel*4+3]/255.0*a;
        if(alpha<=0)continue;
        const double dst_alpha=frame.alpha.empty()?1.0:frame.alpha[pixel]/255.0;
        const double out_alpha=alpha+dst_alpha*(1.0-alpha);
        for(int ch=0;ch<3;++ch)frame.rgb[pixel*3+ch]=clamp_byte((raster.rgba[pixel*4+ch]*alpha+frame.rgb[pixel*3+ch]*dst_alpha*(1.0-alpha))/out_alpha);
        if(!frame.alpha.empty())frame.alpha[pixel]=clamp_byte(out_alpha*255.0);
    }
}

bool write_text_pam(const std::string& path,const GeneratedText& text,int width,int height){
    const auto raster=rasterize_generated_text(text,width,height);std::ofstream f(path_from_utf8(path),std::ios::binary);if(!f)return false;f<<"P7\nWIDTH "<<raster.width<<"\nHEIGHT "<<raster.height<<"\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";f.write(reinterpret_cast<const char*>(raster.rgba.data()),static_cast<std::streamsize>(raster.rgba.size()));return static_cast<bool>(f);
}

void box_blur(Frame& frame,int radius){
    radius=std::clamp(radius,1,16);
    if(frame.rgb.empty()||frame.width<=1||frame.height<=1)return;
    const int w=frame.width,h=frame.height;
    // Preview effects run on one worker thread. Reuse the blur work buffers
    // across frames so dragging an FX slider does not allocate several MB on
    // every preview frame.
    thread_local std::vector<std::uint8_t> tmp;
    thread_local std::vector<std::uint8_t> out;
    tmp.resize(frame.rgb.size());
    out.resize(frame.rgb.size());
    for(int y=0;y<h;++y){
        for(int c=0;c<3;++c){
            int sum=0,count=0;
            for(int x=0;x<=std::min(w-1,radius);++x){sum+=frame.rgb[(static_cast<std::size_t>(y)*w+x)*3+c];++count;}
            for(int x=0;x<w;++x){
                tmp[(static_cast<std::size_t>(y)*w+x)*3+c]=static_cast<std::uint8_t>(sum/std::max(1,count));
                const int remove=x-radius;if(remove>=0){sum-=frame.rgb[(static_cast<std::size_t>(y)*w+remove)*3+c];--count;}
                const int add=x+radius+1;if(add<w){sum+=frame.rgb[(static_cast<std::size_t>(y)*w+add)*3+c];++count;}
            }
        }
    }
    for(int x=0;x<w;++x){
        for(int c=0;c<3;++c){
            int sum=0,count=0;
            for(int y=0;y<=std::min(h-1,radius);++y){sum+=tmp[(static_cast<std::size_t>(y)*w+x)*3+c];++count;}
            for(int y=0;y<h;++y){
                out[(static_cast<std::size_t>(y)*w+x)*3+c]=static_cast<std::uint8_t>(sum/std::max(1,count));
                const int remove=y-radius;if(remove>=0){sum-=tmp[(static_cast<std::size_t>(remove)*w+x)*3+c];--count;}
                const int add=y+radius+1;if(add<h){sum+=tmp[(static_cast<std::size_t>(add)*w+x)*3+c];++count;}
            }
        }
    }
    frame.rgb.swap(out);
}


std::vector<float> luma_plane(const Frame& frame){
    std::vector<float> out(static_cast<std::size_t>(frame.width)*frame.height,0.0f);
    for(std::size_t p=0,i=0;p<out.size();++p,i+=3){
        out[p]=static_cast<float>((0.299*frame.rgb[i]+0.587*frame.rgb[i+1]+0.114*frame.rgb[i+2])/255.0);
    }
    return out;
}

void box_blur_plane(std::vector<float>& plane,int w,int h,int radius){
    if(radius<=0||w<=1||h<=1||plane.empty())return;
    radius=std::clamp(radius,1,24);
    thread_local std::vector<float> tmp;
    thread_local std::vector<float> out;
    tmp.assign(plane.size(),0.0f);out.assign(plane.size(),0.0f);
    for(int y=0;y<h;++y){
        double sum=0.0;int count=0;
        for(int x=0;x<=std::min(w-1,radius);++x){sum+=plane[static_cast<std::size_t>(y)*w+x];++count;}
        for(int x=0;x<w;++x){
            tmp[static_cast<std::size_t>(y)*w+x]=static_cast<float>(sum/std::max(1,count));
            const int remove=x-radius;if(remove>=0){sum-=plane[static_cast<std::size_t>(y)*w+remove];--count;}
            const int add=x+radius+1;if(add<w){sum+=plane[static_cast<std::size_t>(y)*w+add];++count;}
        }
    }
    for(int x=0;x<w;++x){
        double sum=0.0;int count=0;
        for(int y=0;y<=std::min(h-1,radius);++y){sum+=tmp[static_cast<std::size_t>(y)*w+x];++count;}
        for(int y=0;y<h;++y){
            out[static_cast<std::size_t>(y)*w+x]=static_cast<float>(sum/std::max(1,count));
            const int remove=y-radius;if(remove>=0){sum-=tmp[static_cast<std::size_t>(remove)*w+x];--count;}
            const int add=y+radius+1;if(add<h){sum+=tmp[static_cast<std::size_t>(add)*w+x];++count;}
        }
    }
    plane.swap(out);
}

float sigmoidf(float x){return 1.0f/(1.0f+std::exp(-x));}

void anime_line_restore_cpu(Frame& frame,const EffectInstance& e,double time){
    if(frame.width<3||frame.height<3||frame.rgb.empty())return;
    const int w=frame.width,h=frame.height;
    const double strength=std::clamp(effect_numeric(e,"strength",time,0.65),0.0,1.5);
    const double threshold=std::clamp(effect_numeric(e,"threshold",time,22.0),1.0,40.0);
    const double line_width=std::clamp(effect_numeric(e,"line_width",time,0.8),0.5,3.0);
    const double hardness=std::clamp(effect_numeric(e,"edge_hardness",time,8.0),2.0,15.0);
    const double color_pull=std::clamp(effect_numeric(e,"color_pull",time,0.85),0.0,1.0);
    const double max_luma=std::clamp(effect_numeric(e,"max_luma",time,230.0),80.0,255.0)/255.0;
    const auto original=frame.rgb;
    auto smooth=luma_plane(frame);box_blur_plane(smooth,w,h,1);
    auto local=smooth;box_blur_plane(local,w,h,3);
    std::vector<float> alpha(static_cast<std::size_t>(w)*h,0.0f);
    for(int y=1;y<h-1;++y)for(int x=1;x<w-1;++x){
        const std::size_t p=static_cast<std::size_t>(y)*w+x;
        const float valley=std::max(0.0f,local[p]-smooth[p]);
        const float gx=(smooth[p+1]-smooth[p-1])*0.5f;
        const float gy=(smooth[p+w]-smooth[p-w])*0.5f;
        const float grad=std::sqrt(gx*gx+gy*gy);
        const float field=(0.78f*valley+0.22f*grad)*255.0f;
        if(smooth[p]>max_luma||field<threshold)continue;
        const float normalized=std::clamp((field-static_cast<float>(threshold))/std::max(6.0f,48.0f-static_cast<float>(threshold)*0.35f),0.0f,1.0f);
        alpha[p]=sigmoidf(static_cast<float>(hardness)*(normalized-0.35f));
    }
    if(line_width>0.65)box_blur_plane(alpha,w,h,std::clamp(static_cast<int>(std::lround(line_width*0.65)),1,2));
    const double gamma=1.0+color_pull*1.5;
    for(std::size_t p=0,i=0;p<alpha.size();++p,i+=3){
        const double a=std::clamp(static_cast<double>(alpha[p])*strength,0.0,1.0);if(a<=1e-5)continue;
        for(int c=0;c<3;++c){
            const double src=original[i+c]/255.0;
            const double ink=std::pow(std::clamp(src,1e-4,1.0),gamma);
            frame.rgb[i+c]=clamp_byte((src*(1.0-a)+ink*a)*255.0);
        }
    }
}

void anime_vector_color_cpu(Frame& frame,const EffectInstance& e,double time){
    if(frame.rgb.empty())return;
    const auto original=frame.rgb;
    const double saturation=std::clamp(effect_numeric(e,"saturation",time,1.25),0.0,2.5);
    const double contrast=std::clamp(effect_numeric(e,"contrast",time,1.10),0.5,2.0);
    const double flatness=std::clamp(effect_numeric(e,"flatness",time,0.65),0.0,1.0);
    const int levels=std::clamp(static_cast<int>(std::lround(effect_numeric(e,"levels",time,48.0))),8,128);
    const double original_blend=std::clamp(effect_numeric(e,"original_blend",time,0.45),0.0,1.0);
    thread_local Frame smoothed;smoothed.width=frame.width;smoothed.height=frame.height;smoothed.rgb=frame.rgb;if(flatness>1e-4)box_blur(smoothed,2);
    for(std::size_t i=0;i+2<frame.rgb.size();i+=3){
        double rgb[3];
        for(int c=0;c<3;++c){
            const double s=smoothed.rgb[i+c]/255.0;
            const double q=std::round(s*levels)/levels;
            rgb[c]=s+(q-s)*flatness;
            rgb[c]=std::clamp((rgb[c]-0.5)*contrast+0.5,0.0,1.0);
        }
        const double luma=0.299*rgb[0]+0.587*rgb[1]+0.114*rgb[2];
        for(int c=0;c<3;++c){
            double v=std::clamp(luma+(rgb[c]-luma)*saturation,0.0,1.0);
            v=v*(1.0-original_blend)+(original[i+c]/255.0)*original_blend;
            frame.rgb[i+c]=clamp_byte(v*255.0);
        }
    }
}

void anime_text_restore_cpu(Frame& frame,const EffectInstance& e,double time){
    if(frame.width<3||frame.height<3||frame.rgb.empty())return;
    const int w=frame.width,h=frame.height;
    const double threshold=std::clamp(effect_numeric(e,"threshold",time,0.10),0.02,0.35);
    const double boost=std::clamp(effect_numeric(e,"boost",time,0.20),0.0,1.0);
    const auto original=frame.rgb;
    auto luma=luma_plane(frame);auto bg=luma;box_blur_plane(bg,w,h,3);
    std::vector<float> edges(luma.size(),0.0f);
    for(int y=1;y<h-1;++y)for(int x=1;x<w-1;++x){const std::size_t p=static_cast<std::size_t>(y)*w+x;const float gx=(luma[p+1]-luma[p-1])*0.5f,gy=(luma[p+w]-luma[p-w])*0.5f;edges[p]=std::sqrt(gx*gx+gy*gy)>0.12f?1.0f:0.0f;}
    box_blur_plane(edges,w,h,8);
    for(std::size_t p=0,i=0;p<luma.size();++p,i+=3){
        const double zone=std::clamp((edges[p]-0.06f)/0.18f,0.0f,1.0f);if(zone<=1e-5)continue;
        const double letter=std::max(0.0,static_cast<double>(luma[p]-bg[p]));
        const double crisp=sigmoidf(static_cast<float>(30.0*(letter-threshold)));
        const double a=std::clamp(zone*crisp,0.0,1.0);if(a<=1e-4)continue;
        for(int c=0;c<3;++c){const double src=original[i+c]/255.0;const double bright=std::clamp(src*(1.0+boost),0.0,1.0);frame.rgb[i+c]=clamp_byte((src*(1.0-a)+bright*a)*255.0);}
    }
}

void anime_paper_texture_cpu(Frame& frame,const EffectInstance& e,double time){
    if(frame.rgb.empty())return;
    const double texture=std::clamp(effect_numeric(e,"texture",time,0.015),0.0,0.5);
    const double warmth=std::clamp(effect_numeric(e,"warmth",time,0.04),0.0,0.5);
    if(texture<=1e-6&&warmth<=1e-6)return;
    const double phase=e.id*0.731+time*0.17;
    for(int y=0;y<frame.height;++y)for(int x=0;x<frame.width;++x){
        const std::size_t i=(static_cast<std::size_t>(y)*frame.width+x)*3;
        const double luma=(0.299*frame.rgb[i]+0.587*frame.rgb[i+1]+0.114*frame.rgb[i+2])/255.0;
        const double low=std::sin(x*0.031+y*0.019+phase)*0.65;
        std::uint32_t h=static_cast<std::uint32_t>((x+17)*73856093u)^static_cast<std::uint32_t>((y+31)*19349663u)^static_cast<std::uint32_t>(e.id*83492791u);
        h^=h>>13;h*=1274126177u;const double high=((h&1023)/511.5-1.0)*0.35;
        const double paper=1.0+(low+high)*texture*luma;
        const double r=frame.rgb[i]/255.0*paper*(1.0+warmth*0.5);
        const double g=frame.rgb[i+1]/255.0*paper*(1.0+warmth*0.1);
        const double b=frame.rgb[i+2]/255.0*paper*(1.0-warmth*0.4);
        frame.rgb[i]=clamp_byte(r*255.0);frame.rgb[i+1]=clamp_byte(g*255.0);frame.rgb[i+2]=clamp_byte(b*255.0);
    }
}

void anime_deinterlace_cpu(Frame& frame,const EffectInstance& e,double time){
    if(frame.width<=0||frame.height<3||frame.rgb.empty())return;
    const double strength=std::clamp(effect_numeric(e,"strength",time,1.0),0.0,1.0);if(strength<=1e-6)return;
    const auto original=frame.rgb;const int w=frame.width,h=frame.height;
    for(int y=1;y<h-1;y+=2)for(int x=0;x<w;++x){const std::size_t i=(static_cast<std::size_t>(y)*w+x)*3,up=(static_cast<std::size_t>(y-1)*w+x)*3,dn=(static_cast<std::size_t>(y+1)*w+x)*3;for(int c=0;c<3;++c){const double interp=(original[up+c]+original[dn+c])*0.5;frame.rgb[i+c]=clamp_byte(original[i+c]*(1.0-strength)+interp*strength);}}
}

void apply_motion_cpu(Frame& frame,const VideoMotionSample& motion){
    if(frame.rgb.empty()||frame.width<=0||frame.height<=0)return;
    const double zoom=std::clamp(motion.zoom,1.0,8.0);
    const double cx=std::clamp(motion.center_x,0.0,1.0),cy=std::clamp(motion.center_y,0.0,1.0);
    const double angle=-motion.rotation*3.14159265358979323846/180.0;
    if(std::abs(zoom-1.0)<1e-8&&std::abs(motion.rotation)<1e-8&&std::abs(cx-0.5)<1e-8&&std::abs(cy-0.5)<1e-8)return;
    const int w=frame.width,h=frame.height;const auto source=frame.rgb;std::fill(frame.rgb.begin(),frame.rgb.end(),std::uint8_t{0});
    const auto source_alpha=frame.alpha;
    frame.alpha.assign(static_cast<std::size_t>(w)*h,0);
    const double cs=std::cos(angle),sn=std::sin(angle);const double dcx=(w-1)*0.5,dcy=(h-1)*0.5;
    const double crop_x=(w*zoom-w)*cx,crop_y=(h*zoom-h)*cy;
    for(int y=0;y<h;++y){for(int x=0;x<w;++x){
        const double rx=(x-dcx)*cs-(y-dcy)*sn+dcx;
        const double ry=(x-dcx)*sn+(y-dcy)*cs+dcy;
        const double sx=(rx+crop_x)/zoom,sy=(ry+crop_y)/zoom;
        const int ix=static_cast<int>(std::lround(sx)),iy=static_cast<int>(std::lround(sy));
        if(ix<0||ix>=w||iy<0||iy>=h)continue;
        const auto si=(static_cast<std::size_t>(iy)*w+ix)*3,di=(static_cast<std::size_t>(y)*w+x)*3;
        frame.rgb[di]=source[si];frame.rgb[di+1]=source[si+1];frame.rgb[di+2]=source[si+2];
        frame.alpha[di/3]=source_alpha.empty()?255:source_alpha[si/3];
    }}
}

enum class PixelOpKind { BlackWhite, BrightnessContrast, Invert, Saturation, Sepia, ColorBalance, ColorCorrector, FillLight, HueShift, Levels };
struct PixelOp {
    PixelOpKind kind=PixelOpKind::Invert;
    double a=0.0,b=0.0,c=0.0,d=0.0,e=0.0;
};

bool make_pixel_op(const EffectInstance& e,double event_time,PixelOp& out){
    if(e.definition_id=="koralo.black_white"){
        out={PixelOpKind::BlackWhite,std::clamp(effect_numeric(e,"amount",event_time,1.0),0.0,1.0),0,0};return true;
    }
    if(e.definition_id=="koralo.brightness_contrast"){
        out={PixelOpKind::BrightnessContrast,effect_numeric(e,"brightness",event_time,0.0),effect_numeric(e,"contrast",event_time,0.0),0};return true;
    }
    if(e.definition_id=="koralo.invert"){out={PixelOpKind::Invert,0,0,0};return true;}
    if(e.definition_id=="koralo.saturation"){
        out={PixelOpKind::Saturation,std::max(0.0,effect_numeric(e,"amount",event_time,1.0)),0,0};return true;
    }
    if(e.definition_id=="koralo.sepia"){out={PixelOpKind::Sepia,0,0,0};return true;}
    if(e.definition_id=="koralo.color_balance"){
        out={PixelOpKind::ColorBalance,effect_numeric(e,"red",event_time,0.0)*96.0,effect_numeric(e,"green",event_time,0.0)*96.0,effect_numeric(e,"blue",event_time,0.0)*96.0};return true;
    }
    if(e.definition_id=="koralo.color_corrector"){
        out={PixelOpKind::ColorCorrector,effect_numeric(e,"brightness",event_time,0.0),std::max(0.05,effect_numeric(e,"gamma",event_time,1.0)),std::max(0.0,effect_numeric(e,"saturation",event_time,1.0))};return true;
    }
    if(e.definition_id=="koralo.fill_light"){
        out={PixelOpKind::FillLight,effect_numeric(e,"amount",event_time,0.15)*255.0,0,0};return true;
    }
    if(e.definition_id=="koralo.hue_shift"){
        out={PixelOpKind::HueShift,std::clamp(effect_numeric(e,"degrees",event_time,0.0),-180.0,180.0),0,0,0,0};return true;
    }
    if(e.definition_id=="koralo.levels"){
        double ib=std::clamp(effect_numeric(e,"input_black",event_time,0.0),0.0,1.0);
        double iw=std::clamp(effect_numeric(e,"input_white",event_time,1.0),0.0,1.0);
        if(iw<ib+0.001)iw=std::min(1.0,ib+0.001);
        out={PixelOpKind::Levels,ib,iw,std::clamp(effect_numeric(e,"gamma",event_time,1.0),0.1,4.0),std::clamp(effect_numeric(e,"output_black",event_time,0.0),0.0,1.0),std::clamp(effect_numeric(e,"output_white",event_time,1.0),0.0,1.0)};return true;
    }
    return false;
}

void apply_pixel_ops(Frame& frame,const std::vector<PixelOp>& ops){
    if(ops.empty())return;
    bool all_single_channel = true;
    for (const auto& op : ops) {
        if (op.kind == PixelOpKind::BlackWhite || op.kind == PixelOpKind::Saturation ||
            op.kind == PixelOpKind::Sepia || op.kind == PixelOpKind::ColorCorrector ||
            op.kind == PixelOpKind::HueShift) {
            all_single_channel = false;
            break;
        }
    }
    if (all_single_channel) {
        std::uint8_t lut_r[256], lut_g[256], lut_b[256];
        for (int v = 0; v < 256; ++v) {
            double r = v, g = v, b = v;
            for (const auto& op : ops) {
                switch(op.kind){
                    case PixelOpKind::BrightnessContrast:{auto f=[&](double val){return ((val/255.0-0.5)*(1.0+op.b)+0.5+op.a)*255.0;};r=f(r);g=f(g);b=f(b);break;}
                    case PixelOpKind::Invert:r=255.0-r;g=255.0-g;b=255.0-b;break;
                    case PixelOpKind::ColorBalance:r+=op.a;g+=op.b;b+=op.c;break;
                    case PixelOpKind::FillLight:r+=op.a;g+=op.a;b+=op.a;break;
                    case PixelOpKind::Levels:{auto f=[&](double val){double n=std::clamp((val/255.0-op.a)/std::max(1e-6,op.b-op.a),0.0,1.0);const double ga=std::max(1e-4,std::abs(op.c));n=std::pow(n,1.0/ga);return (op.d+n*(op.e-op.d))*255.0;};r=f(r);g=f(g);b=f(b);break;}
                    default: break;
                }
                r=std::clamp(r,0.0,255.0);g=std::clamp(g,0.0,255.0);b=std::clamp(b,0.0,255.0);
            }
            lut_r[v] = static_cast<std::uint8_t>(std::lround(r));
            lut_g[v] = static_cast<std::uint8_t>(std::lround(g));
            lut_b[v] = static_cast<std::uint8_t>(std::lround(b));
        }
        for (std::size_t i = 0; i + 2 < frame.rgb.size(); i += 3) {
            frame.rgb[i] = lut_r[frame.rgb[i]];
            frame.rgb[i + 1] = lut_g[frame.rgb[i + 1]];
            frame.rgb[i + 2] = lut_b[frame.rgb[i + 2]];
        }
        return;
    }
    for(std::size_t i=0;i+2<frame.rgb.size();i+=3){
        double r=frame.rgb[i],g=frame.rgb[i+1],b=frame.rgb[i+2];
        for(const auto& op:ops){
            switch(op.kind){
                case PixelOpKind::BlackWhite:{const double y=0.2126*r+0.7152*g+0.0722*b;r+=(y-r)*op.a;g+=(y-g)*op.a;b+=(y-b)*op.a;break;}
                case PixelOpKind::BrightnessContrast:{auto f=[&](double v){return ((v/255.0-0.5)*(1.0+op.b)+0.5+op.a)*255.0;};r=f(r);g=f(g);b=f(b);break;}
                case PixelOpKind::Invert:r=255.0-r;g=255.0-g;b=255.0-b;break;
                case PixelOpKind::Saturation:{const double y=0.2126*r+0.7152*g+0.0722*b;r=y+(r-y)*op.a;g=y+(g-y)*op.a;b=y+(b-y)*op.a;break;}
                case PixelOpKind::Sepia:{const double rr=.393*r+.769*g+.189*b,gg=.349*r+.686*g+.168*b,bb=.272*r+.534*g+.131*b;r=rr;g=gg;b=bb;break;}
                case PixelOpKind::ColorBalance:r+=op.a;g+=op.b;b+=op.c;break;
                case PixelOpKind::ColorCorrector:{const double ga=std::max(1e-4,std::abs(op.b));double rf=std::pow(std::clamp(r/255.0+op.a,0.0,1.0),1.0/ga),gf=std::pow(std::clamp(g/255.0+op.a,0.0,1.0),1.0/ga),bf=std::pow(std::clamp(b/255.0+op.a,0.0,1.0),1.0/ga);const double y=.2126*rf+.7152*gf+.0722*bf;r=(y+(rf-y)*op.c)*255.0;g=(y+(gf-y)*op.c)*255.0;b=(y+(bf-y)*op.c)*255.0;break;}
                case PixelOpKind::FillLight:r+=op.a;g+=op.a;b+=op.a;break;
                case PixelOpKind::HueShift:{
                    double rf=r/255.0,gf=g/255.0,bf=b/255.0;const double mx=std::max({rf,gf,bf}),mn=std::min({rf,gf,bf}),delta=mx-mn;double h=0.0,sat=mx<=1e-12?0.0:delta/mx,val=mx;
                    if(delta>1e-12){if(mx==rf)h=60.0*std::fmod((gf-bf)/delta,6.0);else if(mx==gf)h=60.0*((bf-rf)/delta+2.0);else h=60.0*((rf-gf)/delta+4.0);if(h<0)h+=360.0;}
                    h=std::fmod(h+op.a+360.0,360.0);const double c=val*sat,x=c*(1.0-std::abs(std::fmod(h/60.0,2.0)-1.0)),m=val-c;double rr=0,gg=0,bb=0;
                    if(h<60){rr=c;gg=x;}else if(h<120){rr=x;gg=c;}else if(h<180){gg=c;bb=x;}else if(h<240){gg=x;bb=c;}else if(h<300){rr=x;bb=c;}else{rr=c;bb=x;}r=(rr+m)*255.0;g=(gg+m)*255.0;b=(bb+m)*255.0;break;}
                case PixelOpKind::Levels:{auto f=[&](double v){double n=std::clamp((v/255.0-op.a)/std::max(1e-6,op.b-op.a),0.0,1.0);const double ga=std::max(1e-4,std::abs(op.c));n=std::pow(n,1.0/ga);return (op.d+n*(op.e-op.d))*255.0;};r=f(r);g=f(g);b=f(b);break;}
            }
            r=std::clamp(r,0.0,255.0);g=std::clamp(g,0.0,255.0);b=std::clamp(b,0.0,255.0);
        }
        frame.rgb[i]=static_cast<std::uint8_t>(std::lround(r));frame.rgb[i+1]=static_cast<std::uint8_t>(std::lround(g));frame.rgb[i+2]=static_cast<std::uint8_t>(std::lround(b));
    }
}


void vignette_cpu(Frame& frame,const EffectInstance& e,double time){
    if(frame.rgb.empty()||frame.width<=1||frame.height<=1)return;
    const double amount=std::clamp(effect_numeric(e,"amount",time,0.45),0.0,1.0);
    const double softness=std::clamp(effect_numeric(e,"softness",time,0.55),0.05,1.0);
    if(amount<=1e-6)return;
    const double cx=(frame.width-1)*0.5,cy=(frame.height-1)*0.5;
    const double sx=std::max(1.0,cx),sy=std::max(1.0,cy);
    const double inner=std::clamp(1.0-softness,0.0,0.95);
    for(int y=0;y<frame.height;++y)for(int x=0;x<frame.width;++x){
        const double dx=(x-cx)/sx,dy=(y-cy)/sy;const double r=std::sqrt(dx*dx+dy*dy)/std::sqrt(2.0);
        const double t=std::clamp((r-inner)/std::max(1e-6,1.0-inner),0.0,1.0);const double smooth=t*t*(3.0-2.0*t);const double gain=1.0-amount*smooth;
        const auto i=(static_cast<std::size_t>(y)*frame.width+x)*3u;frame.rgb[i]=clamp_byte(frame.rgb[i]*gain);frame.rgb[i+1]=clamp_byte(frame.rgb[i+1]*gain);frame.rgb[i+2]=clamp_byte(frame.rgb[i+2]*gain);
    }
}

void pixelate_cpu(Frame& frame,const EffectInstance& e,double time){
    if(frame.rgb.empty())return;
    const int block=std::clamp(static_cast<int>(std::lround(effect_numeric(e,"block",time,12.0))),1,96);
    if(block<=1)return;
    const int w=frame.width,h=frame.height;
    for(int by=0;by<h;by+=block)for(int bx=0;bx<w;bx+=block){
        std::uint64_t sr=0,sg=0,sb=0,count=0;const int ey=std::min(h,by+block),ex=std::min(w,bx+block);
        for(int y=by;y<ey;++y)for(int x=bx;x<ex;++x){const auto i=(static_cast<std::size_t>(y)*w+x)*3u;sr+=frame.rgb[i];sg+=frame.rgb[i+1];sb+=frame.rgb[i+2];++count;}
        if(!count)continue;
        const auto r=static_cast<std::uint8_t>(sr/count),g=static_cast<std::uint8_t>(sg/count),b=static_cast<std::uint8_t>(sb/count);
        for(int y=by;y<ey;++y)for(int x=bx;x<ex;++x){const auto i=(static_cast<std::size_t>(y)*w+x)*3u;frame.rgb[i]=r;frame.rgb[i+1]=g;frame.rgb[i+2]=b;}
    }
}

void letterbox_cpu(Frame& frame,const EffectInstance& e,double time){
    if(frame.rgb.empty())return;
    const double frac=std::clamp(effect_numeric(e,"size",time,0.10),0.0,0.35);
    const int bar=std::clamp(static_cast<int>(std::lround(frame.height*frac)),0,frame.height/2);
    if(bar<=0)return;
    const int w=frame.width,h=frame.height;for(int y=0;y<bar;++y)for(int x=0;x<w;++x){auto a=(static_cast<std::size_t>(y)*w+x)*3u;auto b=(static_cast<std::size_t>(h-1-y)*w+x)*3u;frame.rgb[a]=frame.rgb[a+1]=frame.rgb[a+2]=0;frame.rgb[b]=frame.rgb[b+1]=frame.rgb[b+2]=0;}
}

void scanlines_cpu(Frame& frame,const EffectInstance& e,double time){
    if(frame.rgb.empty())return;
    const double strength=std::clamp(effect_numeric(e,"strength",time,0.25),0.0,1.0);
    const int spacing=std::clamp(static_cast<int>(std::lround(effect_numeric(e,"spacing",time,2.0))),2,12);
    if(strength<=1e-6)return;
    const double gain=1.0-strength;for(int y=spacing-1;y<frame.height;y+=spacing)for(int x=0;x<frame.width;++x){const auto i=(static_cast<std::size_t>(y)*frame.width+x)*3u;frame.rgb[i]=clamp_byte(frame.rgb[i]*gain);frame.rgb[i+1]=clamp_byte(frame.rgb[i+1]*gain);frame.rgb[i+2]=clamp_byte(frame.rgb[i+2]*gain);}
}

void rgb_split_cpu(Frame& frame,const EffectInstance& e,double time){
    if(frame.rgb.empty())return;
    const int dx=std::clamp(static_cast<int>(std::lround(effect_numeric(e,"horizontal",time,6.0))),-64,64);
    const int dy=std::clamp(static_cast<int>(std::lround(effect_numeric(e,"vertical",time,0.0))),-64,64);
    if(dx==0&&dy==0)return;
    const int w=frame.width,h=frame.height;const auto src=frame.rgb;auto clampx=[&](int x){return std::clamp(x,0,w-1);};auto clampy=[&](int y){return std::clamp(y,0,h-1);};
    for(int y=0;y<h;++y)for(int x=0;x<w;++x){const auto di=(static_cast<std::size_t>(y)*w+x)*3u;const auto ri=(static_cast<std::size_t>(clampy(y+dy))*w+clampx(x+dx))*3u;const auto bi=(static_cast<std::size_t>(clampy(y-dy))*w+clampx(x-dx))*3u;frame.rgb[di]=src[ri];frame.rgb[di+1]=src[di+1];frame.rgb[di+2]=src[bi+2];}
}

void lens_distortion_cpu(Frame& frame,const EffectInstance& e,double time){
    if(frame.rgb.empty()||frame.width<2||frame.height<2)return;
    const double k1=std::clamp(effect_numeric(e,"k1",time,0.0),-1.0,1.0);
    const double k2=std::clamp(effect_numeric(e,"k2",time,0.0),-1.0,1.0);
    if(std::abs(k1)<1e-8&&std::abs(k2)<1e-8)return;
    const double cx=std::clamp(effect_numeric(e,"center_x",time,0.5),0.0,1.0)*(frame.width-1),cy=std::clamp(effect_numeric(e,"center_y",time,0.5),0.0,1.0)*(frame.height-1);
    const double sx=std::max(1.0,frame.width*0.5),sy=std::max(1.0,frame.height*0.5);
    const int w=frame.width,h=frame.height;
    const auto src=frame.rgb;
    std::vector<std::uint8_t> dst(src.size(),std::uint8_t{0});
    for(int y=0;y<h;++y)for(int x=0;x<w;++x){const double nx=(x-cx)/sx,ny=(y-cy)/sy,r2=nx*nx+ny*ny,factor=1.0+k1*r2+k2*r2*r2;const double fx=cx+nx*factor*sx,fy=cy+ny*factor*sy;const int ix=static_cast<int>(std::lround(fx)),iy=static_cast<int>(std::lround(fy));if(ix<0||iy<0||ix>=w||iy>=h)continue;const auto si=(static_cast<std::size_t>(iy)*w+ix)*3u,di=(static_cast<std::size_t>(y)*w+x)*3u;dst[di]=src[si];dst[di+1]=src[si+1];dst[di+2]=src[si+2];}
    frame.rgb.swap(dst);
}

void glow_cpu(Frame& frame,const EffectInstance& e,double time){
    if(frame.rgb.empty())return;
    const int radius=std::clamp(static_cast<int>(std::lround(effect_numeric(e,"radius",time,6.0))),0,30);
    const double strength=std::clamp(effect_numeric(e,"strength",time,0.45),0.0,1.5);
    const double threshold=std::clamp(effect_numeric(e,"threshold",time,0.62),0.0,1.0);
    if(radius<=0||strength<=1e-6)return;
    thread_local Frame bright;bright.width=frame.width;bright.height=frame.height;bright.rgb.resize(frame.rgb.size());
    for(std::size_t i=0;i+2<frame.rgb.size();i+=3){const double l=(0.2126*frame.rgb[i]+0.7152*frame.rgb[i+1]+0.0722*frame.rgb[i+2])/255.0;const double m=std::clamp((l-threshold)/std::max(1e-6,1.0-threshold),0.0,1.0);bright.rgb[i]=clamp_byte(frame.rgb[i]*m);bright.rgb[i+1]=clamp_byte(frame.rgb[i+1]*m);bright.rgb[i+2]=clamp_byte(frame.rgb[i+2]*m);}
    box_blur(bright,std::min(radius,16));const std::size_t gn=std::min(frame.rgb.size(),bright.rgb.size());for(std::size_t i=0;i<gn;++i){const double base=frame.rgb[i]/255.0,gl=bright.rgb[i]/255.0*strength;frame.rgb[i]=clamp_byte((1.0-(1.0-base)*(1.0-std::clamp(gl,0.0,1.0)))*255.0);}
}

bool effect_bool(const EffectInstance& effect,const std::string& id,bool fallback){
    const auto* p=effect_parameter(effect,id);if(!p)return fallback;
    if(const auto* v=std::get_if<bool>(&p->value))return *v;
    if(const auto* v=std::get_if<int>(&p->value))return *v!=0;
    if(const auto* v=std::get_if<double>(&p->value))return *v>=0.5;
    return fallback;
}

void ensure_alpha(Frame& frame){
    const std::size_t count=static_cast<std::size_t>(std::max(0,frame.width))*static_cast<std::size_t>(std::max(0,frame.height));
    if(frame.alpha.size()!=count)frame.alpha.assign(count,std::uint8_t{255});
}

void chroma_key_cpu(Frame& frame,const EffectInstance& e,double time){
    if(frame.rgb.empty()||frame.width<=0||frame.height<=0)return;
    // Older KoraLo projects used similarity/blend and a fixed green key. Keep
    // those projects visually stable while the new control surface mirrors the
    // observed VEGAS OFX parameters.
    const bool legacy=effect_parameter(e,"similarity")!=nullptr;
    const double kr=legacy?0.0:std::clamp(effect_numeric(e,"color_r",time,0.0),0.0,1.0);
    const double kg=legacy?1.0:std::clamp(effect_numeric(e,"color_g",time,1.0),0.0,1.0);
    const double kb=legacy?0.0:std::clamp(effect_numeric(e,"color_b",time,0.0),0.0,1.0);
    double low=legacy?std::clamp(effect_numeric(e,"similarity",time,0.18),0.0,1.0):std::clamp(effect_numeric(e,"low_threshold",time,0.08),0.0,1.0);
    double high=legacy?std::clamp(low+effect_numeric(e,"blend",time,0.08),0.0,1.0):std::clamp(effect_numeric(e,"high_threshold",time,0.32),0.0,1.0);
    if(high<low+1e-4)high=std::min(1.0,low+1e-4);
    const double blur=legacy?0.0:std::clamp(effect_numeric(e,"blur_amount",time,0.0),0.0,1.0);
    const bool mask_only=!legacy&&effect_bool(e,"show_mask_only",false);

    auto chroma=[](double r,double g,double b){
        const double y=0.299*r+0.587*g+0.114*b;
        return std::pair<double,double>{0.5+0.564*(b-y),0.5+0.713*(r-y)};
    };
    const auto [key_cb,key_cr]=chroma(kr,kg,kb);
    const std::size_t pixels=static_cast<std::size_t>(frame.width)*frame.height;
    std::vector<float> matte(pixels,1.0f);
    for(std::size_t p=0,i=0;p<pixels;++p,i+=3){
        const double r=frame.rgb[i]/255.0,g=frame.rgb[i+1]/255.0,b=frame.rgb[i+2]/255.0;
        const auto [cb,cr]=chroma(r,g,b);
        // Chroma-only distance keeps neutral luminance differences from
        // incorrectly protecting green-screen shadows/highlights.
        const double d=std::clamp(std::hypot(cb-key_cb,cr-key_cr)/0.75,0.0,1.0);
        const double u=std::clamp((d-low)/std::max(1e-6,high-low),0.0,1.0);
        matte[p]=static_cast<float>(u*u*(3.0-2.0*u));
    }
    if(blur>1e-4){
        const int radius=std::clamp(static_cast<int>(std::lround(blur*12.0)),1,12);
        box_blur_plane(matte,frame.width,frame.height,radius);
    }
    if(mask_only){
        frame.alpha.clear();
        for(std::size_t p=0,i=0;p<pixels;++p,i+=3){const auto m=clamp_byte(matte[p]*255.0);frame.rgb[i]=frame.rgb[i+1]=frame.rgb[i+2]=m;}
        return;
    }
    ensure_alpha(frame);
    for(std::size_t p=0;p<pixels;++p)frame.alpha[p]=clamp_byte((frame.alpha[p]/255.0)*matte[p]*255.0);
}

void apply_effects_cpu(Frame& frame,const std::vector<EffectInstance>& effects,double event_time){
    if(frame.rgb.empty()||frame.width<=0||frame.height<=0)return;
    const std::size_t expected_rgb=static_cast<std::size_t>(frame.width)*frame.height*3;
    if(frame.rgb.size()!=expected_rgb)frame.rgb.resize(expected_rgb);
    std::vector<PixelOp> pixel_ops;pixel_ops.reserve(8);
    auto flush_pixel_ops=[&]{apply_pixel_ops(frame,pixel_ops);pixel_ops.clear();};
    auto each_pixel=[&](auto&& fn){for(std::size_t i=0;i+2<frame.rgb.size();i+=3)fn(frame.rgb[i],frame.rgb[i+1],frame.rgb[i+2]);};
    for(const auto& e:effects){if(!e.enabled)continue;
        PixelOp op;if(make_pixel_op(e,event_time,op)){pixel_ops.push_back(op);continue;}
        flush_pixel_ops();
        if(apply_native_effect(frame,e,event_time))continue;
        if(e.definition_id=="koralo.chroma_key"){
            chroma_key_cpu(frame,e,event_time);
        }else if(e.definition_id=="koralo.glow"){
            glow_cpu(frame,e,event_time);
        }else if(e.definition_id=="koralo.vignette"){
            vignette_cpu(frame,e,event_time);
        }else if(e.definition_id=="koralo.pixelate"){
            pixelate_cpu(frame,e,event_time);
        }else if(e.definition_id=="koralo.lens_distortion"){
            lens_distortion_cpu(frame,e,event_time);
        }else if(e.definition_id=="koralo.rgb_split"){
            rgb_split_cpu(frame,e,event_time);
        }else if(e.definition_id=="koralo.letterbox"){
            letterbox_cpu(frame,e,event_time);
        }else if(e.definition_id=="koralo.scanlines"){
            scanlines_cpu(frame,e,event_time);
        }else if(e.definition_id=="koralo.add_noise"||e.definition_id=="koralo.film_grain"){
            const double amount=effect_numeric(e,"amount",event_time,e.definition_id=="koralo.film_grain"?10.0:8.0);if(amount<=0.001)continue;
            std::uint32_t state=static_cast<std::uint32_t>(0x9e3779b9u+e.id*2654435761u+static_cast<std::uint32_t>(std::llround(event_time*1000.0)));
            const double amp=std::clamp(amount*0.75,0.0,64.0);each_pixel([&](auto& r,auto& g,auto& b){state^=state<<13;state^=state>>17;state^=state<<5;const double n=(static_cast<int>(state&255)-127)/127.0*amp;r=clamp_byte(r+n);g=clamp_byte(g+n);b=clamp_byte(b+n);});
        }else if(e.definition_id=="koralo.border"){
            const int t=std::clamp(static_cast<int>(std::lround(effect_numeric(e,"thickness",event_time,6.0))),1,std::max(1,std::min(frame.width,frame.height)/3));for(int y=0;y<frame.height;++y)for(int x=0;x<frame.width;++x)if(x<t||y<t||x>=frame.width-t||y>=frame.height-t){const auto i=(static_cast<std::size_t>(y)*frame.width+x)*3;frame.rgb[i]=frame.rgb[i+1]=frame.rgb[i+2]=255;}
        }else if(e.definition_id=="koralo.chroma_blur"||e.definition_id=="koralo.defocus"||e.definition_id=="koralo.gaussian_blur"){
            const double def=e.definition_id=="koralo.defocus"?4.0:(e.definition_id=="koralo.chroma_blur"?2.0:0.0);const int radius=std::clamp(static_cast<int>(std::ceil(effect_numeric(e,"radius",event_time,def))),0,16);if(radius>0)box_blur(frame,radius);
        }else if(e.definition_id=="koralo.anime_line_restore"){
            anime_line_restore_cpu(frame,e,event_time);
        }else if(e.definition_id=="koralo.anime_vector_color"){
            anime_vector_color_cpu(frame,e,event_time);
        }else if(e.definition_id=="koralo.anime_text_restore"){
            anime_text_restore_cpu(frame,e,event_time);
        }else if(e.definition_id=="koralo.anime_paper_texture"){
            anime_paper_texture_cpu(frame,e,event_time);
        }else if(e.definition_id=="koralo.anime_detail_sharpen"||e.definition_id=="koralo.sharpen"){
            const double fallback=e.definition_id=="koralo.anime_detail_sharpen"?0.5:1.0;
            const double max_amount=e.definition_id=="koralo.anime_detail_sharpen"?2.0:4.0;
            const double amount=std::clamp(effect_numeric(e,"amount",event_time,fallback),0.0,max_amount);if(amount<=0.001)continue;thread_local Frame blurred;blurred.width=frame.width;blurred.height=frame.height;blurred.rgb=frame.rgb;box_blur(blurred,1);const std::size_t sn=std::min(frame.rgb.size(),blurred.rgb.size());for(std::size_t i=0;i<sn;++i)frame.rgb[i]=clamp_byte(frame.rgb[i]+amount*(static_cast<int>(frame.rgb[i])-static_cast<int>(blurred.rgb[i])));
        }else if(e.definition_id=="koralo.anime_deinterlace"){
            anime_deinterlace_cpu(frame,e,event_time);
        }
    }
    flush_pixel_ops();
}

void append_audio_effects(std::ostringstream& filter,const std::vector<EffectInstance>& effects){
    auto db_to_linear=[](double db){return std::pow(10.0,db/20.0);};
    for(const auto& e:effects){if(!e.enabled)continue;
        if(e.definition_id=="koralo.audio.echo"){
            const double delay=std::clamp(effect_numeric(e,"delay_ms",0.0,320.0),20.0,2000.0);
            const double decay=std::clamp(effect_numeric(e,"decay",0.0,0.38),0.0,0.9);
            const double wet=std::clamp(effect_numeric(e,"wet",0.0,0.45),0.0,1.0);
            filter<<",aecho="<<std::clamp(1.0-wet*0.25,0.2,1.0)<<":"<<std::clamp(0.35+wet*0.55,0.0,1.0)<<":"<<delay<<":"<<decay;
        }else if(e.definition_id=="koralo.audio.reverb"){
            const double room=std::clamp(effect_numeric(e,"room",0.0,0.55),0.0,1.0),decay=std::clamp(effect_numeric(e,"decay",0.0,0.45),0.0,0.9),wet=std::clamp(effect_numeric(e,"wet",0.0,0.35),0.0,1.0);
            const int d1=static_cast<int>(25+room*45),d2=static_cast<int>(55+room*90),d3=static_cast<int>(95+room*150);
            filter<<",aecho="<<std::clamp(1.0-wet*0.2,0.3,1.0)<<":"<<std::clamp(0.30+wet*0.55,0.0,1.0)<<":"<<d1<<"|"<<d2<<"|"<<d3<<":"<<decay*0.72<<"|"<<decay*0.50<<"|"<<decay*0.34;
        }else if(e.definition_id=="koralo.audio.chorus"){
            const double depth=std::clamp(effect_numeric(e,"depth",0.0,0.35),0.0,1.0),speed=std::clamp(effect_numeric(e,"speed",0.0,1.2),0.1,5.0),mix=std::clamp(effect_numeric(e,"mix",0.0,0.45),0.0,1.0);
            const double d1=35.0+depth*18.0,d2=52.0+depth*22.0;
            filter<<",chorus="<<std::clamp(1.0-mix*0.35,0.25,1.0)<<":"<<std::clamp(0.25+mix*0.55,0.0,1.0)<<":"<<d1<<"|"<<d2<<":"<<(0.25+depth*0.35)<<"|"<<(0.20+depth*0.28)<<":"<<speed<<"|"<<(speed*0.73)<<":"<<(1.5+depth*2.5)<<"|"<<(2.0+depth*3.0);
        }else if(e.definition_id=="koralo.audio.flanger"){
            const double delay=std::clamp(effect_numeric(e,"delay",0.0,5.0),0.0,30.0),depth=std::clamp(effect_numeric(e,"depth",0.0,3.0),0.0,10.0),feedback=std::clamp(effect_numeric(e,"feedback",0.0,0.25),-0.8,0.8),speed=std::clamp(effect_numeric(e,"speed",0.0,0.5),0.1,5.0);
            filter<<",flanger=delay="<<delay<<":depth="<<depth<<":regen="<<(feedback*95.0)<<":width=65:speed="<<speed;
        }else if(e.definition_id=="koralo.audio.compressor"){
            const double threshold=db_to_linear(std::clamp(effect_numeric(e,"threshold_db",0.0,-18.0),-60.0,0.0));
            const double ratio=std::clamp(effect_numeric(e,"ratio",0.0,4.0),1.0,20.0),attack=std::clamp(effect_numeric(e,"attack_ms",0.0,20.0),1.0,200.0),release=std::clamp(effect_numeric(e,"release_ms",0.0,250.0),20.0,2000.0),makeup=db_to_linear(std::clamp(effect_numeric(e,"makeup_db",0.0,2.0),0.0,24.0));
            filter<<",acompressor=threshold="<<threshold<<":ratio="<<ratio<<":attack="<<attack<<":release="<<release<<":makeup="<<makeup;
        }else if(e.definition_id=="koralo.audio.limiter"){
            filter<<",alimiter=limit="<<std::clamp(effect_numeric(e,"limit",0.0,0.95),0.0625,1.0)<<":attack="<<std::clamp(effect_numeric(e,"attack_ms",0.0,5.0),0.1,80.0)<<":release="<<std::clamp(effect_numeric(e,"release_ms",0.0,50.0),1.0,8000.0)<<"";
        }else if(e.definition_id=="koralo.audio.noise_gate"){
            const double threshold=db_to_linear(std::clamp(effect_numeric(e,"threshold_db",0.0,-38.0),-80.0,-2.0));
            filter<<",agate=threshold="<<threshold<<":ratio="<<std::clamp(effect_numeric(e,"ratio",0.0,8.0),1.0,20.0)<<":attack="<<std::clamp(effect_numeric(e,"attack_ms",0.0,8.0),1.0,100.0)<<":release="<<std::clamp(effect_numeric(e,"release_ms",0.0,180.0),10.0,1500.0);
        }else if(e.definition_id=="koralo.audio.eq3"){
            const double low=std::clamp(effect_numeric(e,"low_db",0.0,0.0),-18.0,18.0),mid=std::clamp(effect_numeric(e,"mid_db",0.0,0.0),-18.0,18.0),high=std::clamp(effect_numeric(e,"high_db",0.0,0.0),-18.0,18.0);
            if(std::abs(low)>1e-6)filter<<",bass=f=120:g="<<low;
            if(std::abs(mid)>1e-6)filter<<",equalizer=f=1000:t=q:w=1:g="<<mid;
            if(std::abs(high)>1e-6)filter<<",treble=f=6500:g="<<high;
        }else if(e.definition_id=="koralo.audio.highpass"){
            filter<<",highpass=f="<<std::clamp(effect_numeric(e,"frequency",0.0,90.0),20.0,4000.0);
        }else if(e.definition_id=="koralo.audio.lowpass"){
            filter<<",lowpass=f="<<std::clamp(effect_numeric(e,"frequency",0.0,16000.0),200.0,20000.0);
        }else if(e.definition_id=="koralo.audio.distortion"){
            const double drive=db_to_linear(std::clamp(effect_numeric(e,"drive_db",0.0,8.0),0.0,30.0)),mix=std::clamp(effect_numeric(e,"mix",0.0,0.6),0.0,1.0);
            const double effective=1.0+(drive-1.0)*mix;filter<<",volume="<<effective<<",asoftclip=type=tanh:threshold="<<std::clamp(1.0/effective,0.02,1.0)<<":output="<<std::clamp(1.0/std::sqrt(effective),0.1,1.0);
        }else if(e.definition_id=="koralo.audio.tremolo"){
            filter<<",tremolo=f="<<std::clamp(effect_numeric(e,"frequency",0.0,5.0),0.1,20.0)<<":d="<<std::clamp(effect_numeric(e,"depth",0.0,0.5),0.0,1.0);
        }else if(e.definition_id=="koralo.audio.stereo_width"){
            filter<<",aformat=channel_layouts=stereo,stereotools=mlev=1:slev="<<std::clamp(effect_numeric(e,"width",0.0,1.0),0.015625,2.0);
        }else if(e.definition_id=="koralo.audio.gain"){
            filter<<",volume="<<db_to_linear(std::clamp(effect_numeric(e,"gain_db",0.0,0.0),-48.0,24.0));
        }else if(e.definition_id=="koralo.audio.deesser"){
            const double f=std::clamp(effect_numeric(e,"frequency",0.0,6500.0),3000.0,12000.0),g=std::clamp(effect_numeric(e,"reduction_db",0.0,-6.0),-18.0,0.0);filter<<",equalizer=f="<<f<<":t=q:w=1.6:g="<<g;
        }else if(e.definition_id=="koralo.audio.bass_boost"){
            filter<<",bass=f="<<std::clamp(effect_numeric(e,"frequency",0.0,110.0),40.0,300.0)<<":g="<<std::clamp(effect_numeric(e,"gain_db",0.0,5.0),-12.0,18.0);
        }else if(e.definition_id=="koralo.audio.vocal_presence"){
            filter<<",equalizer=f="<<std::clamp(effect_numeric(e,"frequency",0.0,3200.0),1000.0,7000.0)<<":t=q:w=1:g="<<std::clamp(effect_numeric(e,"gain_db",0.0,3.0),-12.0,12.0);
        }else if(e.definition_id=="koralo.audio.normalize"){
            const double strength=std::clamp(effect_numeric(e,"strength",0.0,0.6),0.0,1.0);if(strength>0.001)filter<<",dynaudnorm=f="<<static_cast<int>(std::lround(500.0-350.0*strength))<<":g="<<static_cast<int>(std::lround(5.0+20.0*strength));
        }
    }
}

void append_render_effects(std::ostringstream& filter,const std::vector<EffectInstance>& effects,const std::string& time_expr){
    for(const auto&e:effects){if(!e.enabled)continue;
        if(e.definition_id=="koralo.glow"){
            const double radius=std::clamp(effect_numeric(e,"radius",0.0,6.0),0.0,30.0),strength=std::clamp(effect_numeric(e,"strength",0.0,0.45),0.0,1.5);
            if(radius>0.001&&strength>0.001)filter<<",unsharp=7:7:"<<(-std::min(1.5,strength*0.8));
        }else if(e.definition_id=="koralo.hue_shift"){
            const auto*p=effect_parameter(e,"degrees");const std::string deg=p?effect_param_expr(*p,time_expr,0.0):"0";filter<<",hue=h='"<<deg<<"'";
        }else if(e.definition_id=="koralo.levels"){
            const double ib=std::clamp(effect_numeric(e,"input_black",0.0,0.0),0.0,0.999),iw=std::clamp(effect_numeric(e,"input_white",0.0,1.0),ib+0.001,1.0),ob=std::clamp(effect_numeric(e,"output_black",0.0,0.0),0.0,1.0),ow=std::clamp(effect_numeric(e,"output_white",0.0,1.0),0.0,1.0),ga=std::clamp(effect_numeric(e,"gamma",0.0,1.0),0.1,4.0);
            filter<<",colorlevels=rimin="<<ib<<":gimin="<<ib<<":bimin="<<ib<<":rimax="<<iw<<":gimax="<<iw<<":bimax="<<iw<<":romin="<<ob<<":gomin="<<ob<<":bomin="<<ob<<":romax="<<ow<<":gomax="<<ow<<":bomax="<<ow;
            if(std::abs(ga-1.0)>1e-4)filter<<",eq=gamma="<<ga;
        }else if(e.definition_id=="koralo.vignette"){
            const double amount=std::clamp(effect_numeric(e,"amount",0.0,0.45),0.0,1.0),soft=std::clamp(effect_numeric(e,"softness",0.0,0.55),0.05,1.0);if(amount>0.001){const double angle=3.14159265358979323846/std::max(2.0,6.5-4.0*amount-1.5*(1.0-soft));filter<<",vignette=angle="<<angle<<":eval=frame";}
        }else if(e.definition_id=="koralo.pixelate"){
            const int block=std::clamp(static_cast<int>(std::lround(effect_numeric(e,"block",0.0,12.0))),1,96);if(block>1)filter<<",pixelize=w="<<block<<":h="<<block<<":mode=avg";
        }else if(e.definition_id=="koralo.lens_distortion"){
            filter<<",lenscorrection=cx="<<std::clamp(effect_numeric(e,"center_x",0.0,0.5),0.0,1.0)<<":cy="<<std::clamp(effect_numeric(e,"center_y",0.0,0.5),0.0,1.0)<<":k1="<<std::clamp(effect_numeric(e,"k1",0.0,0.0),-1.0,1.0)<<":k2="<<std::clamp(effect_numeric(e,"k2",0.0,0.0),-1.0,1.0)<<":i=bilinear";
        }else if(e.definition_id=="koralo.rgb_split"){
            const int dx=std::clamp(static_cast<int>(std::lround(effect_numeric(e,"horizontal",0.0,6.0))),-64,64),dy=std::clamp(static_cast<int>(std::lround(effect_numeric(e,"vertical",0.0,0.0))),-64,64);if(dx||dy)filter<<",rgbashift=rh="<<dx<<":rv="<<dy<<":bh="<<(-dx)<<":bv="<<(-dy)<<":edge=smear";
        }else if(e.definition_id=="koralo.letterbox"){
            const double size=std::clamp(effect_numeric(e,"size",0.0,0.10),0.0,0.35);if(size>0.0001)filter<<",drawbox=x=0:y=0:w=iw:h=ih*"<<size<<":color=black:t=fill,drawbox=x=0:y=ih*(1-"<<size<<"):w=iw:h=ih*"<<size<<":color=black:t=fill";
        }else if(e.definition_id=="koralo.scanlines"){
            const double strength=std::clamp(effect_numeric(e,"strength",0.0,0.25),0.0,1.0);const int spacing=std::clamp(static_cast<int>(std::lround(effect_numeric(e,"spacing",0.0,2.0))),2,12);if(strength>0.001)filter<<",geq=r='r(X,Y)*(1-"<<strength<<"*eq(mod(Y,"<<spacing<<"),"<<(spacing-1)<<"))':g='g(X,Y)*(1-"<<strength<<"*eq(mod(Y,"<<spacing<<"),"<<(spacing-1)<<"))':b='b(X,Y)*(1-"<<strength<<"*eq(mod(Y,"<<spacing<<"),"<<(spacing-1)<<"))'";
        }else if(e.definition_id=="koralo.add_noise"){
            const double a=effect_numeric(e,"amount",0.0,8.0);if(a>0.001)filter<<",noise=alls="<<a<<":allf=t";
        }else if(e.definition_id=="koralo.black_white"){
            const auto*ap=effect_parameter(e,"amount");const std::string a=ap?effect_param_expr(*ap,time_expr,1.0):"1";filter<<",eq=saturation='1-("<<a<<")':eval=frame";
        }else if(e.definition_id=="koralo.border"){
            const double t=effect_numeric(e,"thickness",0.0,6.0);if(t>0.001)filter<<",drawbox=x=0:y=0:w=iw:h=ih:color=white:t="<<t;
        }else if(e.definition_id=="koralo.brightness_contrast"){
            const auto*bp=effect_parameter(e,"brightness");const auto*cp=effect_parameter(e,"contrast");
            const std::string b=bp?effect_param_expr(*bp,time_expr,0.0):"0";const std::string c=cp?effect_param_expr(*cp,time_expr,0.0):"0";
            filter<<",eq=brightness='"<<b<<"':contrast='1+("<<c<<")':eval=frame";
        }else if(e.definition_id=="koralo.chroma_blur"){
            const double r=effect_numeric(e,"radius",0.0,2.0);if(r>0.001)filter<<",gblur=sigma="<<r;
        }else if(e.definition_id=="koralo.chroma_key"){
            const bool legacy=effect_parameter(e,"similarity")!=nullptr;
            const double kr=legacy?0.0:std::clamp(effect_numeric(e,"color_r",0.0,0.0),0.0,1.0);
            const double kg=legacy?1.0:std::clamp(effect_numeric(e,"color_g",0.0,1.0),0.0,1.0);
            const double kb=legacy?0.0:std::clamp(effect_numeric(e,"color_b",0.0,0.0),0.0,1.0);
            double low=legacy?std::clamp(effect_numeric(e,"similarity",0.0,0.18),0.001,1.0):std::clamp(effect_numeric(e,"low_threshold",0.0,0.08),0.001,1.0);
            double high=legacy?std::clamp(low+effect_numeric(e,"blend",0.0,0.08),low,1.0):std::clamp(effect_numeric(e,"high_threshold",0.0,0.32),low,1.0);
            const double blur=legacy?0.0:std::clamp(effect_numeric(e,"blur_amount",0.0,0.0),0.0,1.0);
            const int rr=std::clamp(static_cast<int>(std::lround(kr*255.0)),0,255),gg=std::clamp(static_cast<int>(std::lround(kg*255.0)),0,255),bb=std::clamp(static_cast<int>(std::lround(kb*255.0)),0,255);
            std::ostringstream hex;hex<<"0x"<<std::uppercase<<std::hex<<std::setw(2)<<std::setfill('0')<<rr<<std::setw(2)<<gg<<std::setw(2)<<bb<<std::dec;
            // FFmpeg chromakey operates in chroma space and produces real alpha.
            // Blend models the threshold transition; blur extends that soft edge
            // until the future branched compositor can blur alpha independently.
            const double blend=std::clamp((high-low)+blur*0.18,0.0,1.0);
            filter<<",chromakey="<<hex.str()<<":"<<low<<":"<<blend;
            if(!legacy&&effect_bool(e,"show_mask_only",false))filter<<",alphaextract,format=rgba";
        }else if(e.definition_id=="koralo.color_balance"){
            filter<<",colorbalance=rs="<<effect_numeric(e,"red",0.0,0.0)<<":gs="<<effect_numeric(e,"green",0.0,0.0)<<":bs="<<effect_numeric(e,"blue",0.0,0.0);
        }else if(e.definition_id=="koralo.color_corrector"){
            const auto*bp=effect_parameter(e,"brightness");const auto*gp=effect_parameter(e,"gamma");const auto*sp=effect_parameter(e,"saturation");
            const std::string br=bp?effect_param_expr(*bp,time_expr,0.0):"0",ga=gp?effect_param_expr(*gp,time_expr,1.0):"1",sa=sp?effect_param_expr(*sp,time_expr,1.0):"1";
            filter<<",eq=brightness='"<<br<<"':gamma='"<<ga<<"':saturation='"<<sa<<"':eval=frame";
        }else if(e.definition_id=="koralo.defocus"||e.definition_id=="koralo.gaussian_blur"){
            const double r=effect_numeric(e,"radius",0.0,e.definition_id=="koralo.defocus"?4.0:0.0);if(r>0.001)filter<<",gblur=sigma="<<r;
        }else if(e.definition_id=="koralo.fill_light"){
            const auto*ap=effect_parameter(e,"amount");const std::string a=ap?effect_param_expr(*ap,time_expr,0.15):"0.15";filter<<",eq=brightness='"<<a<<"':eval=frame";
        }else if(e.definition_id=="koralo.film_grain"){
            const double a=effect_numeric(e,"amount",0.0,10.0);if(a>0.001)filter<<",noise=alls="<<a<<":allf=t";
        }else if(e.definition_id=="koralo.invert"){
            filter<<",negate";
        }else if(e.definition_id=="koralo.saturation"){
            const auto*ap=effect_parameter(e,"amount");const std::string a=ap?effect_param_expr(*ap,time_expr,1.0):"1";filter<<",eq=saturation='"<<a<<"':eval=frame";
        }else if(e.definition_id=="koralo.sepia"){
            filter<<",colorchannelmixer=.393:.769:.189:0:.349:.686:.168:0:.272:.534:.131";
        }else if(e.definition_id=="koralo.anime_line_restore"){
            const double strength=std::clamp(effect_numeric(e,"strength",0.0,0.65),0.0,1.5);
            const double width=std::clamp(effect_numeric(e,"line_width",0.0,0.8),0.5,3.0);
            if(strength>0.001)filter<<",hqdn3d="<<std::max(0.0,0.35*width)<<":"<<std::max(0.0,0.35*width)<<":1.0:1.0,unsharp=5:5:"<<std::min(2.0,strength*1.35);
        }else if(e.definition_id=="koralo.anime_vector_color"){
            const double flat=std::clamp(effect_numeric(e,"flatness",0.0,0.65),0.0,1.0);
            const int levels=std::clamp(static_cast<int>(std::lround(effect_numeric(e,"levels",0.0,48.0))),8,128);
            const double contrast=std::clamp(effect_numeric(e,"contrast",0.0,1.10),0.5,2.0);
            const double saturation=std::clamp(effect_numeric(e,"saturation",0.0,1.25),0.0,2.5);
            if(flat>0.001)filter<<",gblur=sigma="<<(flat*1.8);
            filter<<",lutrgb=r='floor(val*"<<levels<<"/256)*256/"<<levels<<"':g='floor(val*"<<levels<<"/256)*256/"<<levels<<"':b='floor(val*"<<levels<<"/256)*256/"<<levels<<"'";
            filter<<",eq=contrast="<<contrast<<":saturation="<<saturation;
        }else if(e.definition_id=="koralo.anime_text_restore"){
            const double boost=std::clamp(effect_numeric(e,"boost",0.0,0.20),0.0,1.0);if(boost>0.001)filter<<",unsharp=3:3:"<<(0.35+boost*1.8);
        }else if(e.definition_id=="koralo.anime_paper_texture"){
            const double texture=std::clamp(effect_numeric(e,"texture",0.0,0.015),0.0,0.5),warmth=std::clamp(effect_numeric(e,"warmth",0.0,0.04),0.0,0.5);
            if(texture>0.001)filter<<",noise=alls="<<(texture*28.0)<<":allf=t";
            if(warmth>0.001)filter<<",colorbalance=rs="<<(warmth*0.22)<<":gs="<<(warmth*0.05)<<":bs="<<(-warmth*0.18);
        }else if(e.definition_id=="koralo.anime_detail_sharpen"){
            const double amount=std::clamp(effect_numeric(e,"amount",0.0,0.5),0.0,2.0);if(amount>0.001)filter<<",unsharp=5:5:"<<amount;
        }else if(e.definition_id=="koralo.anime_deinterlace"){
            const double strength=std::clamp(effect_numeric(e,"strength",0.0,1.0),0.0,1.0);if(strength>0.001)filter<<",bwdif=mode=0:parity=-1:deint=0";
        }else if(e.definition_id=="koralo.sharpen"){
            filter<<",unsharp=5:5:"<<effect_numeric(e,"amount",0.0,1.0);
        }
    }
}
}


std::string MediaEngine::locate_binary(const std::string& name){return locate_media_binary(name);}

MediaEngine::MediaEngine():ffmpeg_(locate_binary("ffmpeg")),ffprobe_(locate_binary("ffprobe")),ffplay_(locate_binary("ffplay")),cache_(144){
    gpu_=detect_ffmpeg_gpu_backend(ffmpeg_);
    worker_=std::thread([this]{worker_loop();});
    thumbnail_worker_=std::thread([this]{thumbnail_worker_loop();});
    meter_worker_=std::thread([this]{meter_worker_loop();});
    preparation_worker_=std::thread([this]{preparation_worker_loop();});
}
MediaEngine::~MediaEngine(){
    audio_recorder_.terminate();stop_audio_preview();stop_=true;playback_controller_.wake();thumbnail_cv_.notify_all();meter_cv_.notify_all();preparation_cv_.notify_all();
    if(worker_.joinable())worker_.join();
    if(thumbnail_worker_.joinable())thumbnail_worker_.join();
    if(meter_worker_.joinable())meter_worker_.join();
    if(preparation_worker_.joinable())preparation_worker_.join();
}
std::string MediaEngine::diagnostics()const{std::ostringstream s;s<<"ffmpeg: "<<(ffmpeg_.empty()?"missing":ffmpeg_)<<"\nffprobe: "<<(ffprobe_.empty()?"missing":ffprobe_)<<"\nffplay: "<<(ffplay_.empty()?"missing (audio preview disabled)":ffplay_)<<"\nGPU: "<<gpu_.label<<(gpu_enabled_flag_.load()?" (enabled)":" (disabled)")<<"\nNative CUDA FSR: "<<(fsr_cuda_available()?"available":"unavailable (CPU fallback)");return s.str();}

std::optional<MediaItem> MediaEngine::probe(const std::string& path,std::string* error)const{
    return probe_media_file(ffprobe_,path,error);
}

namespace {
std::string file_stamp(const std::string& path){std::error_code error;const auto file=path_from_utf8(path);const auto modified=fs::last_write_time(file,error);if(error)return {};const auto size=fs::file_size(file,error);return error?std::string{}:std::to_string(modified.time_since_epoch().count())+":"+std::to_string(size);}
}
std::string MediaEngine::frame_key(const PreviewRequest& r)const{
    if(!r.layers.empty()){
        std::ostringstream key;key << "layers:" << r.width << 'x' << r.height;
        for(const auto& layer:r.layers){const auto part=frame_key(layer);key << '|' << part.size() << ':' << part;}
        PreviewRequest output;output.effects=r.output_effects;output.effect_time=r.output_effect_time;
        key << "|output:" << frame_key(output);return key.str();
    }
    std::ostringstream s;s<<r.preserve_alpha<<'|'<<r.path<<'|'<<file_stamp(r.path)<<'|'<<std::fixed<<std::setprecision(3)<<r.time<<'|'<<r.width<<'x'<<r.height<<"|a:"<<std::setprecision(4)<<std::clamp(r.opacity,0.0,1.0);
    if(r.vector_source.enabled)s<<"|vector:"<<vector_scene_fingerprint(r.vector_source);
    if(r.background_vector_source.enabled)s<<"|bg-vector:"<<vector_scene_fingerprint(r.background_vector_source);
    if(r.apply_motion)s<<"|m:"<<std::setprecision(5)<<r.motion.center_x<<','<<r.motion.center_y<<','<<r.motion.zoom<<','<<r.motion.rotation;
    if(!r.effects.empty()){s<<"|fx@"<<std::setprecision(3)<<r.effect_time;for(const auto&e:r.effects){s<<':'<<e.id<<':'<<e.definition_id<<':'<<e.enabled;for(const auto&p:e.parameters){s<<':'<<p.id<<'='<<effect_value_string(p.value)<<'@'<<p.animated;for(const auto&k:p.keys)s<<'['<<std::setprecision(5)<<k.time<<','<<k.value<<','<<static_cast<int>(k.interpolation)<<']';}}}
    if(!r.output_effects.empty()){PreviewRequest master;master.effects=r.output_effects;master.effect_time=r.output_effect_time;s<<"|master:"<<frame_key(master);}
    if(r.has_text_overlay){const auto&g=r.text_overlay;s<<"|text:"<<g.text<<':'<<g.font_family<<':'<<g.font_size<<':'<<g.bold<<':'<<g.italic<<':'<<g.alignment<<':'<<g.color_r<<':'<<g.color_g<<':'<<g.color_b<<':'<<g.color_a<<':'<<g.scale<<':'<<g.location_x<<':'<<g.location_y<<':'<<g.anchor<<':'<<g.outline<<':'<<g.outline_size<<':'<<g.outline_r<<':'<<g.outline_g<<':'<<g.outline_b<<':'<<g.outline_a<<':'<<g.shadow<<':'<<g.shadow_x<<':'<<g.shadow_y<<':'<<g.shadow_opacity<<':'<<g.animation<<':'<<r.text_opacity<<':'<<r.text_event_time<<':'<<r.text_event_duration;}
    if(r.has_background){s<<"|bg:"<<r.background_path<<'@'<<std::setprecision(4)<<r.background_time<<"|a:"<<r.background_opacity;if(r.background_apply_motion)s<<"|m:"<<r.background_motion.center_x<<','<<r.background_motion.center_y<<','<<r.background_motion.zoom<<','<<r.background_motion.rotation;for(const auto&e:r.background_effects){s<<"|fx:"<<e.id<<':'<<e.definition_id<<':'<<e.enabled;for(const auto&p:e.parameters)s<<':'<<p.id<<'='<<effect_value_string(p.value);}}
    return s.str();
}

std::string MediaEngine::source_frame_key(const PreviewRequest& r,const std::string& input_path)const{const double source_time=is_still_image_path(input_path)?0.0:r.time;std::ostringstream s;s<<input_path<<"|source|"<<file_stamp(input_path)<<'|'<<std::fixed<<std::setprecision(4)<<source_time<<'|'<<r.width<<'x'<<r.height;return s.str();}

void MediaEngine::apply_preview_opacity(Frame& frame,double opacity){
    const double a=std::clamp(opacity,0.0,1.0);
    if(a>=0.999999||frame.rgb.empty())return;
    // Keep opacity as alpha while compositing preview layers. If the frame is
    // eventually shown without a background it is flattened over black.
    ensure_alpha(frame);
    for(auto& alpha:frame.alpha)alpha=clamp_byte(alpha*a);
}

namespace {
void flatten_alpha_over_black(Frame& frame){
    if(frame.alpha.empty()||frame.rgb.empty())return;
    const std::size_t pixels=std::min(frame.alpha.size(),frame.rgb.size()/3u);
    for(std::size_t p=0,i=0;p<pixels;++p,i+=3){const double a=frame.alpha[p]/255.0;frame.rgb[i]=clamp_byte(frame.rgb[i]*a);frame.rgb[i+1]=clamp_byte(frame.rgb[i+1]*a);frame.rgb[i+2]=clamp_byte(frame.rgb[i+2]*a);}
    frame.alpha.clear();
}

void composite_over(Frame& background,const Frame& foreground){
    if(background.rgb.empty()||foreground.rgb.empty()||background.width!=foreground.width||background.height!=foreground.height)return;
    const std::size_t pixels=std::min(background.rgb.size(),foreground.rgb.size())/3u;
    ensure_alpha(background);
    for(std::size_t p=0,i=0;p<pixels;++p,i+=3){
        const double a=foreground.alpha.empty()?1.0:foreground.alpha[p]/255.0;
        const double ba=background.alpha.empty()?1.0:background.alpha[p]/255.0;
        const double out_a=a+ba*(1.0-a);
        background.alpha[p]=clamp_byte(out_a*255.0);
        if(out_a<=1e-9){background.rgb[i]=background.rgb[i+1]=background.rgb[i+2]=0;continue;}
        for(int ch=0;ch<3;++ch){const double fg=foreground.rgb[i+ch]/255.0,bg=background.rgb[i+ch]/255.0;const double out=(fg*a+bg*ba*(1.0-a))/out_a;background.rgb[i+ch]=clamp_byte(out*255.0);}
    }
    // Retain accumulated alpha until the final flatten over the output background.
}
}

void MediaEngine::apply_preview_pipeline(Frame& frame,const PreviewRequest& req){
    const bool text_layer=req.preserve_alpha&&req.has_text_overlay;
    if(text_layer)composite_generated_text(frame,req.text_overlay,req.text_opacity,req.text_event_time,req.text_event_duration);
    if(req.apply_motion)apply_motion_cpu(frame,req.motion);
    if(!req.effects.empty())apply_effects_cpu(frame,req.effects,req.effect_time);
    apply_preview_opacity(frame,req.opacity);
    if(req.has_text_overlay&&!text_layer)composite_generated_text(frame,req.text_overlay,req.text_opacity,req.text_event_time,req.text_event_duration);
}

Frame MediaEngine::decode_frame(const PreviewRequest& req,std::string* error)const{
    Frame f;
    auto finish=[&](Frame frame)->Frame{
        if(req.has_background&&!frame.rgb.empty()){
            PreviewRequest bg;bg.path=req.background_path;bg.time=req.background_time;bg.vector_source=req.background_vector_source;bg.width=req.width;bg.height=req.height;bg.allow_prepared_cache=req.background_allow_prepared_cache;bg.apply_motion=req.background_apply_motion;bg.motion=req.background_motion;bg.effects=req.background_effects;bg.effect_time=req.background_effect_time;bg.opacity=req.background_opacity;bg.continuous=false;
            std::string bg_error;Frame background=decode_frame(bg,&bg_error);
            if(!background.rgb.empty()){composite_over(background,frame);frame=std::move(background);}
        }
        if(!req.preserve_alpha)flatten_alpha_over_black(frame);
        apply_effects_cpu(frame,req.output_effects,req.output_effect_time);if(!req.preserve_alpha)flatten_alpha_over_black(frame);if(error&&!frame.rgb.empty())error->clear();return frame;
    };
    if(req.vector_source.enabled){f=render_vector_scene(req.vector_source,req.width,req.height,req.time);apply_preview_pipeline(f,req);return finish(std::move(f));}
    if(req.path.empty()){f.width=req.width;f.height=req.height;f.rgb.assign(static_cast<std::size_t>(req.width)*req.height*3u,0);if(req.preserve_alpha&&req.has_text_overlay)f.alpha.assign(static_cast<std::size_t>(req.width)*req.height,0);apply_preview_pipeline(f,req);return finish(std::move(f));}
    if(ffmpeg_.empty()){if(error)*error="ffmpeg missing";return f;}
    const std::string input_path=req.allow_prepared_cache?prepared_preview_path(req.path):req.path;
    const std::string base_key=source_frame_key(req,input_path);
    {
        std::lock_guard cache_lock(cache_mutex_);
        if(auto cached=source_cache_.get(base_key))f=std::move(*cached);
    }
    if(f.rgb.empty()){
        source_decode_count_.fetch_add(1);
        std::string tmp=temp_name(".ppm");
        std::ostringstream time;time<<std::fixed<<std::setprecision(6)<<(is_still_image_path(input_path)?0.0:std::max(0.0,req.time));
        std::ostringstream filter;filter<<"scale="<<req.width<<":"<<req.height<<":force_original_aspect_ratio=decrease,pad="<<req.width<<":"<<req.height<<":(ow-iw)/2:(oh-ih)/2:black";
        auto run_decode=[&](bool hw){
            std::vector<std::string> args{"-nostdin","-hide_banner","-loglevel","error","-threads","2","-filter_threads","2"};
            if(hw&&gpu_enabled_flag_.load()&&gpu_.decode_available)args.insert(args.end(),{"-hwaccel","auto"});
            // FFmpeg 7.x can emit zero frames for a single still image when an
            // input seek of exactly 0 is supplied before the image input. A
            // still has only one source frame anyway, so do not seek it. This
            // also fixes stopped/scrub preview and timeline thumbnails for JPG/
            // PNG images with odd dimensions.
            if(!is_still_image_path(input_path))args.insert(args.end(),{"-ss",time.str()});
            args.insert(args.end(),{"-i",input_path,"-map","0:V:0","-an","-sn","-dn","-frames:v","1","-vf",filter.str(),"-pix_fmt","rgb24","-f","image2","-c:v","ppm","-threads:v","1","-update","1","-y",tmp});
            ProcessOptions execution;execution.cancel=&stop_;execution.timeout_ms=10000;return run_process(ffmpeg_,args,execution);
        };
        const bool hardware=gpu_enabled_flag_.load()&&gpu_.decode_available;
        auto process=run_decode(hardware);
        auto valid_frame=[&]{return process.started&&process.exit_code==0&&read_ppm(tmp,f)&&f.width==req.width&&f.height==req.height;};
        bool decoded=valid_frame();
        // Some GPU decoders exit successfully but produce no usable frame.
        // Retry that case on CPU as well as ordinary process failures.
        if(!decoded&&hardware&&!stop_){f={};process=run_decode(false);decoded=valid_frame();}
        if(!decoded){f={};if(error)*error=process.output.empty()?"frame decode failed":process.output;}
        std::error_code ec;fs::remove(path_from_utf8(tmp),ec);
        if(!f.rgb.empty()){std::lock_guard cache_lock(cache_mutex_);source_cache_.put(base_key,f);}
    }
    apply_preview_pipeline(f,req);return finish(std::move(f));
}

void MediaEngine::reset_preview_stream(){decoder_pool_.reset();layer_decoders_.clear();}

bool MediaEngine::start_preview_stream(DecoderPool& decoder, const PreviewRequest& req,const std::string& input_path,bool hardware,std::string* error){
    decoder.reset();
    const double request_fps=std::clamp(req.requested_fps,1.0,120.0);
    const double source_rate=std::clamp(req.source_rate,0.05,16.0);
    const double output_fps=std::clamp(request_fps/source_rate,1.0,240.0);
    const double step=1.0/output_fps;
    std::ostringstream time,fps;
    time<<std::fixed<<std::setprecision(6)<<std::max(0.0,req.time);
    fps<<std::fixed<<std::setprecision(6)<<output_fps;
    std::ostringstream filter;
    filter<<"fps="<<fps.str()<<",scale="<<req.width<<":"<<req.height
          <<":force_original_aspect_ratio=decrease,pad="<<req.width<<":"<<req.height
          <<":(ow-iw)/2:(oh-ih)/2:black";
    std::vector<std::string> args{"-hide_banner","-loglevel","error","-nostdin","-threads","2","-filter_threads","2"};
    if(hardware&&gpu_enabled_flag_.load()&&gpu_.decode_available)args.insert(args.end(),{"-hwaccel","auto"});
    if(!is_still_image_path(input_path))args.insert(args.end(),{"-ss",time.str()});
    if(is_still_image_path(input_path))args.insert(args.end(),{"-loop","1","-framerate",fps.str()});
    args.insert(args.end(),{"-i",input_path,"-map","0:V:0","-an","-sn","-dn","-vf",filter.str(),"-pix_fmt","rgb24","-c:v","rawvideo","-threads:v","1","-f","rawvideo","pipe:1"});
    const bool started=decoder.start(ffmpeg_,args,input_path,req.width,req.height,step,std::max(0.0,req.time),hardware,error);
    if(started)stream_start_count_.fetch_add(1);
    return started;
}

Frame MediaEngine::decode_stream_source(const PreviewRequest& req,DecoderPool& decoder,std::string* error){
    Frame frame;
    if(req.continuous&&req.vector_source.enabled&&req.has_background&&!req.background_path.empty()&&!req.background_vector_source.enabled){
        PreviewRequest bg;bg.path=req.background_path;bg.time=req.background_time;bg.width=req.width;bg.height=req.height;bg.allow_prepared_cache=req.background_allow_prepared_cache;bg.apply_motion=req.background_apply_motion;bg.motion=req.background_motion;bg.effects=req.background_effects;bg.effect_time=req.background_effect_time;bg.opacity=req.background_opacity;bg.continuous=true;bg.requested_fps=req.requested_fps;bg.source_rate=req.background_source_rate;
        Frame background=decode_stream_frame(bg,error);if(background.rgb.empty())return decode_frame(req,error);Frame foreground=render_vector_scene(req.vector_source,req.width,req.height,req.time);apply_preview_pipeline(foreground,req);composite_over(background,foreground);apply_effects_cpu(background,req.output_effects,req.output_effect_time);flatten_alpha_over_black(background);return background;
    }
    if(ffmpeg_.empty()){if(error)*error="ffmpeg missing";return frame;}
    if(!req.continuous||req.path.empty())return decode_frame(req,error);
    const std::string input_path=req.allow_prepared_cache?prepared_preview_path(req.path):req.path;
    const double request_fps=std::clamp(req.requested_fps,1.0,120.0);
    const double source_rate=std::clamp(req.source_rate,0.05,16.0);
    const double step=1.0/std::clamp(request_fps/source_rate,1.0,240.0);
    auto compatible=[&]{return decoder.compatible(input_path,req.width,req.height,step);};
    auto start=[&](bool hardware){return start_preview_stream(decoder,req,input_path,hardware,error);};
    if(!compatible()){
        const bool hardware=gpu_enabled_flag_.load()&&gpu_.decode_available;
        if(!start(hardware))return frame;
    }else{
        const double delta=req.time-decoder.next_time();
        // Tiny backwards jitter comes from UI scheduling; a real seek or a
        // large dropped-frame gap is cheaper and more accurate to restart.
        if(delta<-step*0.75||delta>0.40){
            if(!start(decoder.hardware()))return frame;
        }
    }

    const std::size_t bytes=static_cast<std::size_t>(req.width)*static_cast<std::size_t>(req.height)*3u;
    auto read_one=[&](Frame& out)->bool{
        out.width=req.width;out.height=req.height;out.rgb.resize(bytes);
        if(!decoder.read_exact(out.rgb.data(),bytes,error,&stop_)){out={};return false;}
        decoder.advance();
        return true;
    };

    // If the UI skipped requests while a decode was in flight, drop only the
    // few stale raw frames still between the decoder and the requested time.
    // This keeps the visible frame aligned to the wall-clock playhead without
    // repeatedly seeking/restarting FFmpeg.
    if(decoder.step()>0.0&&req.time>decoder.next_time()+decoder.step()*0.50){
        const int skip=std::clamp(static_cast<int>(std::llround((req.time-decoder.next_time())/decoder.step())),0,24);
        Frame discarded;
        for(int i=0;i<skip;++i){if(!read_one(discarded))break;}
    }
    if(!read_one(frame)){
        const bool retry_cpu=decoder.hardware();
        decoder.reset();
        if(retry_cpu&&start(false)&&read_one(frame)){
            apply_preview_pipeline(frame,req);
            if(!req.preserve_alpha)flatten_alpha_over_black(frame);
    apply_effects_cpu(frame,req.output_effects,req.output_effect_time);if(!req.preserve_alpha)flatten_alpha_over_black(frame);
            if(error)error->clear();
            return frame;
        }
        return {};
    }
    apply_preview_pipeline(frame,req);
    if(!req.preserve_alpha)flatten_alpha_over_black(frame);
    apply_effects_cpu(frame,req.output_effects,req.output_effect_time);if(!req.preserve_alpha)flatten_alpha_over_black(frame);
    if(error)error->clear();
    return frame;
}

Frame MediaEngine::decode_layers(const PreviewRequest& req,bool streaming,std::string* error){
    Frame composite;composite.width=req.width;composite.height=req.height;
    composite.rgb.assign(static_cast<std::size_t>(req.width)*req.height*3u,0);
    composite.alpha.assign(static_cast<std::size_t>(req.width)*req.height,0);
    // A plain opaque video fills the entire decoded canvas (including padding).
    // Lower layers cannot contribute; do not spend decoders/CPU on hidden clips.
    std::size_t first=0;
    for(std::size_t i=req.layers.size();i>0;--i){
        const auto& layer=req.layers[i-1];
        if(!layer.path.empty()&&!layer.vector_source.enabled&&!layer.has_text_overlay&&
           !layer.apply_motion&&layer.effects.empty()&&layer.opacity>=1.0){first=i-1;break;}
    }
    std::unordered_set<int> active;
    for(std::size_t i=first;i<req.layers.size();++i){const auto& layer=req.layers[i];if(streaming&&layer.continuous&&!layer.path.empty())active.insert(layer.stream_id);}
    for(auto it=layer_decoders_.begin();it!=layer_decoders_.end();){
        if(!active.contains(it->first))it=layer_decoders_.erase(it);else ++it;
    }
    for(std::size_t i=first;i<req.layers.size();++i){
        auto layer=req.layers[i];
        if(stop_)return {};
        layer.width=req.width;layer.height=req.height;layer.preserve_alpha=true;
        Frame frame;
        if(streaming&&layer.continuous&&!layer.path.empty()){
            auto& decoder=layer_decoders_[layer.stream_id];
            if(!decoder)decoder=std::make_unique<DecoderPool>();
            frame=decode_stream_source(layer,*decoder,error);
        }else frame=decode_frame(layer,error);
        if(frame.rgb.empty())return {}; // Never silently publish an incomplete composite.
        composite_over(composite,frame);
    }
    flatten_alpha_over_black(composite);
    apply_effects_cpu(composite,req.output_effects,req.output_effect_time);
    flatten_alpha_over_black(composite);
    return composite;
}

Frame MediaEngine::decode_stream_frame(const PreviewRequest& req,std::string* error){
    if(!req.layers.empty()){decoder_pool_.reset();return decode_layers(req,req.continuous,error);}
    layer_decoders_.clear();
    if(!req.continuous)decoder_pool_.reset();
    return req.continuous?decode_stream_source(req,decoder_pool_,error):decode_frame(req,error);
}

void MediaEngine::request_preview(PreviewRequest req){
    playback_controller_.note_request(req);
    const auto key=frame_key(req);
    playback_controller_.queue_request(std::move(req),key);
}

bool MediaEngine::poll_preview(std::uint64_t& token,Frame& frame,std::string& error){return playback_controller_.poll(token,frame,error);}
void MediaEngine::clear_cache(){{std::lock_guard lk(cache_mutex_);cache_.clear();source_cache_.clear();}{std::lock_guard lk(thumbnail_mutex_);thumbnail_cache_.clear();thumbnail_queue_.clear();thumbnail_pending_.clear();}thumbnail_revision_.fetch_add(1);}
void MediaEngine::clear_composite_cache(){std::lock_guard lk(cache_mutex_);cache_.clear();}

std::optional<Frame> MediaEngine::timeline_thumbnail(const std::string& path,double time,int width,int height){
    width=std::clamp(width,48,192);height=std::clamp(height,24,108);time=std::max(0.0,time);const std::string source=prepared_preview_path(path);
    std::ostringstream key;key<<source<<"|thumb|"<<std::fixed<<std::setprecision(2)<<time<<'|'<<width<<'x'<<height;const std::string k=key.str();
    std::lock_guard lk(thumbnail_mutex_);
    if(auto cached=thumbnail_cache_.get(k))return cached;
    if(thumbnail_pending_.insert(k).second){
        if(thumbnail_queue_.size()>256){thumbnail_pending_.erase(thumbnail_queue_.front().key);thumbnail_queue_.pop_front();}
        thumbnail_queue_.push_back({source,k,time,width,height,{}});
        thumbnail_cv_.notify_one();
    }
    return std::nullopt;
}

std::optional<Frame> MediaEngine::vector_thumbnail(const VectorScene& scene,double time,int width,int height){
    width=std::clamp(width,48,192);height=std::clamp(height,24,108);time=std::max(0.0,time);
    const std::string k="vector-thumb:"+std::to_string(vector_scene_fingerprint(scene))+":"+std::to_string(std::llround(time*100))+":"+std::to_string(width)+":"+std::to_string(height);
    std::lock_guard lk(thumbnail_mutex_);if(auto cached=thumbnail_cache_.get(k))return cached;
    if(thumbnail_pending_.insert(k).second){if(thumbnail_queue_.size()>256){thumbnail_pending_.erase(thumbnail_queue_.front().key);thumbnail_queue_.pop_front();}ThumbnailRequest req;req.key=k;req.time=time;req.width=width;req.height=height;req.vector_source=scene;thumbnail_queue_.push_back(std::move(req));thumbnail_cv_.notify_one();}return std::nullopt;
}
void MediaEngine::thumbnail_worker_loop(){
    while(!stop_){ThumbnailRequest req;{std::unique_lock lk(thumbnail_mutex_);thumbnail_cv_.wait(lk,[&]{return stop_||!thumbnail_queue_.empty();});if(stop_)break;req=std::move(thumbnail_queue_.front());thumbnail_queue_.pop_front();}
        PreviewRequest frame_req;frame_req.path=req.path;frame_req.time=req.time;frame_req.width=req.width;frame_req.height=req.height;frame_req.vector_source=std::move(req.vector_source);
        std::string err;Frame f;try{f=decode_frame(frame_req,&err);}catch(const std::exception& e){err=e.what();}{std::lock_guard lk(thumbnail_mutex_);if(!f.rgb.empty())thumbnail_cache_.put(req.key,std::move(f));thumbnail_pending_.erase(req.key);}thumbnail_revision_.fetch_add(1);
    }
}

void MediaEngine::worker_loop(){
    while(!stop_){PreviewRequest req;if(!playback_controller_.wait_next(stop_,req))break;
        std::string err;Frame f;try{
            if(!req.continuous){
                reset_preview_stream();
                std::lock_guard cache_lock(cache_mutex_);
                if(auto cached=cache_.get(frame_key(req)))f=std::move(*cached);
            }
            if(f.rgb.empty())f=decode_stream_frame(req,&err);
        }catch(const std::exception& e){err=e.what();reset_preview_stream();}
        // Dropping every late playback frame starves the screen indefinitely
        // on a slower decoder/effect chain. Keep completed frames within the
        // same playback epoch; a seek/pause/edit invalidates the old epoch.
        const bool publish=playback_controller_.should_publish(req);
        if(publish&&!f.rgb.empty()){std::lock_guard cache_lock(cache_mutex_);cache_.put(frame_key(req),f);}
        if(publish)playback_controller_.publish_worker_result(req,std::move(f),std::move(err));}
    reset_preview_stream();
}


void MediaEngine::meter_worker_loop(){
    while(!stop_){
        std::string path;
        {
            std::unique_lock lk(meter_mutex_);
            meter_cv_.wait(lk,[&]{return stop_||!meter_queue_.empty();});
            if(stop_)break;
            path=std::move(meter_queue_.front());meter_queue_.pop_front();
        }
        AudioEnvelope envelope;
        if(ffmpeg_.empty())envelope.failed=true;
        else{
            double time=0,left_db=-120,right_db=-120;bool have=false;std::string pending;
            auto flush=[&]{if(!have)return;auto linear=[](double db){return std::isfinite(db)?std::clamp(std::pow(10.0,db/20.0),0.0,1.0):0.0;};envelope.samples.push_back({time,linear(left_db),linear(right_db)});have=false;left_db=right_db=-120;};
            ProcessOptions execution;execution.timeout_ms=0;execution.cancel=&stop_;execution.max_output_bytes=65536;
            execution.on_output=[&](std::string_view chunk){pending.append(chunk);std::size_t end;
                while((end=pending.find('\n'))!=std::string::npos){auto line=pending.substr(0,end);pending.erase(0,end+1);
                    if(line.rfind("frame:",0)==0){flush();auto pos=line.find("pts_time:");if(pos!=std::string::npos)try{time=std::stod(line.substr(pos+9));have=true;}catch(...){} }
                    else if(line.rfind("lavfi.astats.1.Peak_level=",0)==0)try{left_db=std::stod(line.substr(line.find('=')+1));}catch(...){}
                    else if(line.rfind("lavfi.astats.2.Peak_level=",0)==0)try{right_db=std::stod(line.substr(line.find('=')+1));}catch(...){}
                }if(pending.size()>65536)pending.erase(0,pending.size()-65536);
            };
            const auto result=run_process(ffmpeg_,{"-nostdin","-hide_banner","-loglevel","error","-i",path,"-vn","-af","aformat=channel_layouts=stereo,asetnsamples=n=4800:p=1,astats=metadata=1:reset=1,ametadata=print:file=-","-f","null","-"},execution);
            flush();if(!result.started||result.exit_code!=0||envelope.samples.empty())envelope.failed=true;

        }
        {
            std::lock_guard lk(meter_mutex_);meter_cache_[path]=std::move(envelope);meter_pending_.erase(path);
        }
    }
}

AudioMeterLevel MediaEngine::master_meter(const Project& project,double playhead){
    AudioMeterLevel out;
    bool any_solo=false;for(const auto& track:project.tracks)if(track.kind==TrackKind::Audio&&track.solo)any_solo=true;
    double left_sum=0.0,right_sum=0.0;
    for(const auto& clip:project.clips){
        const Track* track=project.track_by_id(clip.track_id);const MediaItem* media=project.media_by_id(clip.media_id());
        if(!track||!media||track->kind!=TrackKind::Audio||!media->has_audio||track->mute||clip.muted||(any_solo&&!track->solo)||!clip.contains(playhead))continue;
        double source=std::max(0.0,clip.source_offset()+(playhead-clip.start)*clip.rate);
        if(clip.loop&&media->duration>0.0)source=std::fmod(source,media->duration);
        double l=0.0,r=0.0;bool found=false;
        {
            std::lock_guard lk(meter_mutex_);
            auto cached=meter_cache_.find(media->path);
            if(cached!=meter_cache_.end()&&!cached->second.failed&&!cached->second.samples.empty()){
                const auto& samples=cached->second.samples;
                auto it=std::lower_bound(samples.begin(),samples.end(),source,[](const MeterSample&s,double t){return s.time<t;});
                if(it==samples.end())it=std::prev(samples.end());
                else if(it!=samples.begin()){auto prev=std::prev(it);if(std::abs(prev->time-source)<std::abs(it->time-source))it=prev;}
                l=it->left;r=it->right;found=true;
            }else if(cached==meter_cache_.end()&&meter_pending_.insert(media->path).second){meter_queue_.push_back(media->path);meter_cv_.notify_one();}
        }
        if(!found)continue;
        double volume=std::max(0.0,clip.gain*track->gain);
        const double event_time=std::clamp(playhead-clip.start,0.0,clip.duration);
        if(clip.fade_in>1e-9)volume*=std::clamp(event_time/clip.fade_in,0.0,1.0);
        if(clip.fade_out>1e-9)volume*=std::clamp((clip.duration-event_time)/clip.fade_out,0.0,1.0);
        l*=volume;r*=volume;
        const double pan=std::clamp(track->pan,-1.0,1.0);if(pan<0)r*=1.0+pan;else if(pan>0)l*=1.0-pan;
        left_sum+=l;right_sum+=r;out.ready=true;
    }
    out.left=std::clamp(left_sum,0.0,1.0);out.right=std::clamp(right_sum,0.0,1.0);return out;
}

namespace {
std::string tempo_filter(double rate){std::ostringstream out;rate=std::clamp(rate,0.01,100.0);while(rate>2){out<<"atempo=2,";rate/=2;}while(rate<0.5){out<<"atempo=0.5,";rate*=2;}out<<"atempo="<<number_expr(rate);return out.str();}
}
bool MediaEngine::sync_audio_preview(const Project& project,double playhead,std::string* error){
    if(ffplay_.empty()){if(error)*error="ffplay is not available; install the full FFmpeg package for audio preview";return false;}
    if(!std::isfinite(playhead)||playhead<0.0){if(error)*error="Invalid audio playhead";return false;}
    // A bounded single-graph session provides one audio device clock even for
    // overlapping clips. The next session is constructed before the end of the
    // current horizon, rather than restarting a separate ffplay per clip.
    const double window_length=audio_mix_horizon_-audio_mix_anchor_;
    const double restart_lead=std::clamp(window_length*0.1,0.05,1.0);
    const bool session_valid=audio_mix_anchor_>=0.0&&
        playhead>=audio_mix_anchor_-0.15&&playhead<audio_mix_horizon_-restart_lead&&
        audio_mix_player_.process.running();
    double anchor=session_valid?audio_mix_anchor_:playhead;
    auto plan=plan_audio_mix(project,anchor);
    const auto serialize=[](const AudioMixPlan& mix){
        return build_audio_mix_lavfi(mix,[](std::ostringstream& filter,
                                  const std::vector<EffectInstance>& effects){
            append_audio_effects(filter,effects);
        });
    };
    auto graph=serialize(plan);
    if(session_valid&&graph&&*graph==audio_mix_signature_)
        return true;
    // A user edit can change volume/FX during playback. Restarting the new
    // graph from the *old* session anchor would replay earlier audio.
    if(session_valid){anchor=playhead;plan=plan_audio_mix(project,anchor);graph=serialize(plan);}
    if(!plan.complete){
        if(error)*error="Too many simultaneous audio sources for bounded preview mix";
        stop_audio_preview();
        return false;
    }
    if(!graph){stop_audio_preview();return true;}
    // If the project changes or the user seeks, replace the whole graph in one
    // operation. Audio sources remain sample-aligned within that one process.
    audio_mix_player_.process.terminate();
    audio_mix_signature_.clear();
    audio_mix_anchor_=-1.0;
    audio_mix_horizon_=-1.0;
    std::string start_error;
    const std::vector<std::string> args{
        "-nodisp","-autoexit","-vn","-sn","-loglevel","quiet",
        "-fflags","nobuffer","-flags","low_delay",
        "-f","lavfi","-i",*graph
    };
    if(!audio_mix_player_.process.start(ffplay_,args,&start_error)){
        if(error)*error=start_error;
        return false;
    }
    audio_mix_signature_=*graph;
    audio_mix_anchor_=anchor;
    audio_mix_horizon_=plan.horizon;
    return true;
}

void MediaEngine::stop_audio_preview(){
    audio_mix_player_.process.terminate();
    audio_mix_signature_.clear();
    audio_mix_anchor_=-1.0;
    audio_mix_horizon_=-1.0;
}

bool MediaEngine::audio_recording_active(){ return audio_recorder_.running(); }

bool MediaEngine::start_audio_recording(const std::string& raw_path,int sample_rate,int channels,std::string* error){
    if(ffmpeg_.empty()){if(error)*error="FFmpeg is required for audio recording";return false;}
    if(audio_recorder_.running()){if(error)*error="Audio recording is already active";return false;}
    audio_recorder_.terminate();
    audio_recording_raw_path_=raw_path;
    audio_recording_sample_rate_=std::clamp(sample_rate,8000,192000);
    audio_recording_channels_=std::clamp(channels,1,2);
    std::error_code ec;fs::create_directories(path_from_utf8(raw_path).parent_path(),ec);fs::remove(path_from_utf8(raw_path),ec);
    auto common=[&](std::vector<std::string> args){
        std::error_code remove_ec;fs::remove(path_from_utf8(raw_path),remove_ec);
        args.insert(args.end(),{"-y","-vn","-ac",std::to_string(audio_recording_channels_),"-ar",std::to_string(audio_recording_sample_rate_),"-acodec","pcm_s16le","-f","s16le",raw_path});
        std::string start_error;
        if(!audio_recorder_.start(ffmpeg_,args,&start_error)){if(error)*error=start_error;return false;}
        std::this_thread::sleep_for(std::chrono::milliseconds(180));
        if(!audio_recorder_.running()){audio_recorder_.terminate();if(error)*error="Audio input device could not be opened";return false;}
        return true;
    };
#ifdef _WIN32
    // DirectShow does not expose a portable "default" alias on all builds, so
    // enumerate devices and use the first audio capture endpoint when needed.
    std::string device;
    const auto listed=run_process(ffmpeg_,{"-hide_banner","-list_devices","true","-f","dshow","-i","dummy"});
    std::istringstream input(listed.output);std::string line;
    while(std::getline(input,line)){
        // Newer FFmpeg builds annotate endpoints with "(audio)" while older
        // builds group them below a DirectShow audio-devices heading. Quoted
        // names are the stable part that can be passed back to dshow.
        if(line.find("(audio)")==std::string::npos && line.find("Alternative name")!=std::string::npos)continue;
        const auto q1=line.find('"');if(q1==std::string::npos)continue;const auto q2=line.find('"',q1+1);if(q2==std::string::npos)continue;
        const std::string candidate=line.substr(q1+1,q2-q1-1);if(!candidate.empty()){device=candidate;break;}
    }
    if(device.empty()){if(error)*error="No DirectShow audio capture device was found";return false;}
    if(!common({"-hide_banner","-loglevel","error","-f","dshow","-i","audio="+device})) return false;
#else
    if(!common({"-hide_banner","-loglevel","error","-f","pulse","-i","default"})){
        audio_recorder_.terminate();
        if(!common({"-hide_banner","-loglevel","error","-f","alsa","-i","default"})) return false;
    }
#endif
    return true;
}

bool MediaEngine::finalize_pcm16le_recording(const std::string& raw_path,const std::string& wav_path,int sample_rate,int channels,double* duration,std::string* error){
    const fs::path raw=path_from_utf8(raw_path);std::error_code ec;
    if(!fs::exists(raw,ec)){if(error)*error="Audio capture produced no data";return false;}
    channels=std::clamp(channels,1,2);sample_rate=std::clamp(sample_rate,8000,192000);
    const std::uint64_t data_size=fs::file_size(raw,ec);if(ec||data_size<2u*static_cast<std::uint64_t>(channels)){if(error)*error="Audio capture is empty";return false;}
    const fs::path out=path_from_utf8(wav_path);fs::create_directories(out.parent_path(),ec);
    std::ifstream in(raw,std::ios::binary);std::ofstream wav(out,std::ios::binary|std::ios::trunc);
    if(!in||!wav){if(error)*error="Could not finalize recorded WAV";return false;}
    auto u16=[&](std::uint16_t v){char b[2]={static_cast<char>(v&0xff),static_cast<char>((v>>8)&0xff)};wav.write(b,2);};
    auto u32=[&](std::uint32_t v){char b[4]={static_cast<char>(v&0xff),static_cast<char>((v>>8)&0xff),static_cast<char>((v>>16)&0xff),static_cast<char>((v>>24)&0xff)};wav.write(b,4);};
    const std::uint32_t data32=static_cast<std::uint32_t>(std::min<std::uint64_t>(data_size,0xffffffffu-44u));
    wav.write("RIFF",4);u32(36u+data32);wav.write("WAVEfmt ",8);u32(16);u16(1);u16(static_cast<std::uint16_t>(channels));u32(static_cast<std::uint32_t>(sample_rate));
    const std::uint32_t byte_rate=static_cast<std::uint32_t>(sample_rate*channels*2);u32(byte_rate);u16(static_cast<std::uint16_t>(channels*2));u16(16);wav.write("data",4);u32(data32);
    std::array<char,65536> buffer{};std::uint64_t left=data32;while(left>0&&in){const std::streamsize want=static_cast<std::streamsize>(std::min<std::uint64_t>(left,buffer.size()));in.read(buffer.data(),want);const auto got=in.gcount();if(got<=0)break;wav.write(buffer.data(),got);left-=static_cast<std::uint64_t>(got);}wav.flush();
    if(duration)*duration=static_cast<double>(data32)/std::max(1u,byte_rate);
    if(!wav){if(error)*error="Failed while writing recorded WAV";return false;}
    return true;
}

bool MediaEngine::stop_audio_recording(const std::string& wav_path,double* duration,std::string* error){
    if(audio_recording_raw_path_.empty()){if(error)*error="No audio recording is active";return false;}
    audio_recorder_.terminate();const std::string raw_path=audio_recording_raw_path_;audio_recording_raw_path_.clear();
    const bool ok=finalize_pcm16le_recording(raw_path,wav_path,audio_recording_sample_rate_,audio_recording_channels_,duration,error);std::error_code ec;fs::remove(path_from_utf8(raw_path),ec);return ok;
}


void MediaEngine::prepare_media(const MediaItem& media){
    if(media.path.empty()||!fs::exists(path_from_utf8(media.path)))return;
    {
        std::lock_guard lock(preparation_mutex_);
        auto& state=preparation_states_[media.path];
        if(state.ready||state.running||state.queued)return;
        state={};state.queued=true;state.progress=0.01;state.stage="Queued for analysis";
        preparation_pending_.insert(media.path);
        preparation_queue_.push_back(media);
    }
    preparation_cv_.notify_one();
}

void MediaEngine::prepare_project(const Project& project){
    for(const auto& media:project.media)prepare_media(media);
}

MediaPreparationProgress MediaEngine::preparation_status(const std::string& path) const{
    std::lock_guard lock(preparation_mutex_);
    auto it=preparation_states_.find(path);
    return it==preparation_states_.end()?MediaPreparationProgress{}:it->second;
}

MediaPreparationProgress MediaEngine::overall_preparation_status() const{
    std::lock_guard lock(preparation_mutex_);
    MediaPreparationProgress out;double sum=0.0;int active_count=0;bool any_failed=false,any_proxy=false;std::string active_stage;
    for(const auto& [_,state]:preparation_states_){
        any_failed|=state.failed;any_proxy|=state.proxy_ready;
        if(state.queued||state.running){sum+=state.progress;++active_count;if(active_stage.empty())active_stage=state.stage;}
    }
    if(active_count==0){out.ready=true;out.progress=1.0;out.failed=any_failed;out.proxy_ready=any_proxy;out.stage=any_failed?"Media cache ready (some sources use original media)":"Media cache ready";return out;}
    out.progress=std::clamp(sum/active_count,0.0,1.0);out.queued=true;out.running=true;out.failed=any_failed;out.proxy_ready=any_proxy;out.stage=active_stage.empty()?"Preparing media cache — please wait":active_stage;return out;
}

std::string MediaEngine::prepared_preview_path(const std::string& original) const{
    std::lock_guard lock(preparation_mutex_);
    auto it=preparation_states_.find(original);
    if(it!=preparation_states_.end()&&it->second.proxy_ready&&!it->second.proxy_path.empty()&&fs::exists(path_from_utf8(it->second.proxy_path)))return it->second.proxy_path;
    return original;
}

void MediaEngine::preparation_worker_loop(){
    while(!stop_){
        MediaItem media;
        {
            std::unique_lock lock(preparation_mutex_);
            preparation_cv_.wait(lock,[&]{return stop_||!preparation_queue_.empty();});
            if(stop_)break;
            media=std::move(preparation_queue_.front());preparation_queue_.pop_front();
            auto& state=preparation_states_[media.path];state.queued=false;state.running=true;state.stage="Analyzing media";state.progress=0.05;
        }
        auto update=[&](double progress,const std::string& stage){std::lock_guard lock(preparation_mutex_);auto& state=preparation_states_[media.path];state.progress=std::clamp(progress,0.0,1.0);state.stage=stage;};
        bool failed=false;
        if(media.has_video&&media.duration>0.0){
            update(0.10,"Building timeline frame cache");
            const bool still_source=is_still_image_path(media.path);const int frame_count=still_source?1:(media.duration>=600.0?24:(media.duration>=120.0?16:10));
            for(int i=0;i<frame_count&&!stop_;++i){
                const double fraction=frame_count<=1?0.0:i/static_cast<double>(frame_count-1);
                const double t=still_source?0.0:std::clamp(media.duration*fraction,0.0,std::max(0.0,media.duration-0.001));
                PreviewRequest req;req.path=media.path;req.time=t;req.width=160;req.height=90;req.allow_prepared_cache=false;
                std::string err;Frame frame=decode_frame(req,&err);
                if(!frame.rgb.empty()){std::ostringstream key;key<<media.path<<"|thumb|"<<std::fixed<<std::setprecision(2)<<t<<"|160x90";std::lock_guard lk(thumbnail_mutex_);thumbnail_cache_.put(key.str(),std::move(frame));thumbnail_revision_.fetch_add(1);}
                update(0.10+0.20*(i+1)/static_cast<double>(frame_count),"Building timeline frame cache");
            }
            // Long-GOP/long-form sources get a persistent seek-friendly preview proxy.
            if(!stop_&&!still_source&&media.duration>=20.0&&ffmpeg_.size()){
                update(0.32,"Building optimized preview cache");
                std::error_code ec;auto cache_dir=cache_directory()/"preview";fs::create_directories(cache_dir,ec);
                std::uint64_t hash=static_cast<std::uint64_t>(std::hash<std::string>{}(media.path));
                try{hash^=static_cast<std::uint64_t>(fs::file_size(path_from_utf8(media.path)))+0x9e3779b97f4a7c15ULL+(hash<<6)+(hash>>2);}catch(...){}
                std::ostringstream filename;auto modified=fs::last_write_time(path_from_utf8(media.path),ec);if(!ec)hash^=static_cast<std::uint64_t>(modified.time_since_epoch().count());ec.clear();filename<<std::hex<<hash<<"_seek_v4.mp4";const std::string proxy=path_to_utf8(cache_dir/filename.str());
                bool proxy_ok=fs::exists(path_from_utf8(proxy));
                if(!proxy_ok){
                    auto build_args=[&](bool hardware){
                        std::vector<std::string> args{"-hide_banner","-loglevel","error","-y"};
                        if(hardware&&gpu_enabled_flag_.load()&&gpu_.decode_available)args.insert(args.end(),{"-hwaccel","auto"});
                        args.insert(args.end(),{"-i",media.path,"-map","0:V:0","-an","-vf","scale='min(960,iw)':-2"});
                        if(hardware&&gpu_enabled_flag_.load()&&gpu_.encode_available)args.insert(args.end(),{"-c:v",gpu_.encoder,"-b:v","4M","-g","1"});
                        else args.insert(args.end(),{"-c:v","libx264","-preset","ultrafast","-tune","fastdecode","-crf","28","-g","1","-keyint_min","1","-sc_threshold","0"});
                        args.insert(args.end(),{"-pix_fmt","yuv420p","-movflags","+faststart",proxy});return args;
                    };
                    auto run_cache=[&](bool hardware){
                        TemporaryFile staged(cache_dir,".mp4");if(!staged.valid())return false;
                        auto args=build_args(hardware);args.back()=path_to_utf8(staged.path());
                        ProcessOptions options;options.timeout_ms=0;options.cancel=&stop_;
                        auto result=run_process(ffmpeg_,args,options);
                        return !stop_&&result.started&&result.exit_code==0&&atomic_replace_file(staged.path(),path_from_utf8(proxy));
                    };
                    const bool try_hw=gpu_enabled_flag_.load()&&(gpu_.decode_available||gpu_.encode_available);proxy_ok=run_cache(try_hw);
                    if(!proxy_ok&&try_hw&&!stop_){fs::remove(path_from_utf8(proxy),ec);update(0.34,"GPU cache path unavailable — retrying on CPU");proxy_ok=run_cache(false);}
                    if(!proxy_ok){failed=true;fs::remove(path_from_utf8(proxy),ec);}
                }
                if(proxy_ok){std::lock_guard lock(preparation_mutex_);auto& state=preparation_states_[media.path];state.proxy_ready=true;state.proxy_path=proxy;}
            }
        }
        if(media.has_audio&&!stop_){
            update(0.86,"Analyzing audio waveform / meters");
            std::lock_guard<std::mutex> lock(meter_mutex_);
            if(meter_cache_.find(media.path)==meter_cache_.end()&&!meter_pending_.contains(media.path)){meter_pending_.insert(media.path);meter_queue_.push_back(media.path);meter_cv_.notify_one();}
        }
        update(0.98,"Finalizing media cache");
        {
            std::lock_guard lock(preparation_mutex_);auto& state=preparation_states_[media.path];state.running=false;state.queued=false;state.ready=true;state.failed=failed;state.progress=1.0;state.stage=failed?"Ready (preview proxy unavailable — using source)":"Media cache ready";preparation_pending_.erase(media.path);
        }
    }
}


bool MediaEngine::create_proxy(const MediaItem& media,const std::string& output,int height,std::string* error)const{
    if(ffmpeg_.empty()){if(error)*error="ffmpeg missing";return false;}
    std::error_code ec;auto destination=fs::absolute(path_from_utf8(output),ec);
    if(ec||!fs::is_directory(destination.parent_path(),ec)){if(error)*error="Proxy directory unavailable";return false;}
    if(fs::equivalent(path_from_utf8(media.path),destination,ec)){if(error)*error="Proxy cannot overwrite source media";return false;}
    TemporaryFile encoded(destination.parent_path(),path_to_utf8(destination.extension()));if(!encoded.valid()){if(error)*error="Could not reserve proxy";return false;}
    auto run_proxy=[&](bool hardware){std::vector<std::string> args{"-hide_banner","-loglevel","error","-y"};if(hardware&&gpu_enabled_flag_.load()&&gpu_.decode_available)args.insert(args.end(),{"-hwaccel","auto"});args.insert(args.end(),{"-i",media.path,"-map","0:V:0","-map","0:a:0?","-vf","scale=-2:"+std::to_string(std::max(240,height))});if(hardware&&gpu_enabled_flag_.load()&&gpu_.encode_available)args.insert(args.end(),{"-c:v",gpu_.encoder,"-b:v","6M","-g","1"});else args.insert(args.end(),{"-c:v","libx264","-preset","veryfast","-tune","fastdecode","-crf","24","-g","1","-keyint_min","1","-sc_threshold","0"});args.insert(args.end(),{"-c:a","aac","-b:a","128k",path_to_utf8(encoded.path())});ProcessOptions execution;execution.timeout_ms=0;execution.cancel=&stop_;return run_process(ffmpeg_,args,execution);};
    auto process=run_proxy(gpu_enabled_flag_.load()&&(gpu_.decode_available||gpu_.encode_available));if((!process.started||process.exit_code!=0)&&!process.cancelled&&!stop_&&gpu_enabled_flag_.load()&&(gpu_.decode_available||gpu_.encode_available))process=run_proxy(false);
    if(!process.started||process.exit_code!=0){if(error)*error=process.output.empty()?"proxy render failed":process.output;return false;}return atomic_replace_file(encoded.path(),destination,error);
}

#include "native_export.inc"

bool MediaEngine::render_project(const Project& p,const std::string& output,std::atomic<double>* progress,std::string* error,const RenderOptions* options,const std::atomic<bool>* cancel)const{
    // Exactly one log/session per user export, reused by all nested stages.
    if (!options || !options->control) {
        RenderControlSession session(path_from_utf8(output),&render_control_live_);
        RenderOptions scoped=options?*options:RenderOptions{};
        scoped.control=&session;
        bool ok=false;
        try { ok=render_project(p,output,progress,error,&scoped,cancel); }
        catch (const std::exception& ex) { if(error)*error=ex.what(); }
        catch (...) { if(error)*error="Unexpected export failure"; }
        session.finish(ok,ok?"Output published after validation":
                       error?*error:std::string("Export failed"));
        return ok;
    }
    if(progress&&(!options||!options->native_prepared))*progress=0;
    if(ffmpeg_.empty()){if(error)*error="ffmpeg missing";return false;}
    const auto planned=plan_render_project(p,output,options,gpu_enabled_flag_.load(),cancel,error);if(!planned)return false;const auto& plan=*planned;
    const int render_width=plan.width,render_height=plan.height;const double render_fps=plan.fps;const int video_kbps=plan.video_bitrate_kbps,audio_kbps=plan.audio_bitrate_kbps;const bool allow_gpu=plan.allow_gpu;const auto& destination=plan.destination;const double total=plan.duration;
    const RenderGraph render_graph=build_render_graph(p,plan);
    if(options&&options->fsr_upscale)return render_fsr_project(p,output,progress,error,*options,cancel);
    if(!plan.native_prepared&&needs_native_export(p))return render_native_project(p,output,progress,error,options,cancel);
    // Kdenlive/MLT-style optimization: do not invoke a compositing graph for
    // an unmodified single source. Unlike stream copy this still honors the
    // requested bitrate/codec/resolution and preserves frame-accurate trims.
    // KORALO_EXPORT_FORCE_GENERIC=1 is for A/B tests and regression triage.
    if(environment_utf8("KORALO_EXPORT_FORCE_GENERIC") != "1") {
      if(const auto direct=choose_direct_export(p,plan,render_graph)) {
        TemporaryFile encoded(destination.parent_path(),path_to_utf8(destination.extension()));
        if(!encoded.valid()){if(error)*error="Could not reserve export file";return false;}
        if(options->control)options->control->record("render_path","direct_single_source");
        std::vector<std::string> base{"-nostdin","-hide_banner","-loglevel","warning",
                                      "-nostats","-progress","pipe:1","-y"};
        if(direct->video->source_offset()>0)
          base.insert(base.end(),{"-ss",number_expr(direct->video->source_offset())});
        base.insert(base.end(),{"-i",direct->media->path,"-map","0:V:0"});
        if(direct->include_audio)base.insert(base.end(),{"-map","0:a:0"});
        else base.push_back("-an");
        if(direct->media->width!=render_width||direct->media->height!=render_height){
          base.insert(base.end(),{"-vf","scale="+std::to_string(render_width)+":"+
              std::to_string(render_height)+":force_original_aspect_ratio=decrease,pad="+
              std::to_string(render_width)+":"+std::to_string(render_height)+
              ":(ow-iw)/2:(oh-ih)/2:black"});
        }
        const auto encode=[&](bool hw){
          auto args=base;
          if(options->lossless_intermediate)args.insert(args.end(),{"-c:v","ffv1","-level","3","-pix_fmt","bgra"});
          else if(hw&&allow_gpu&&gpu_.encode_available)args.insert(args.end(),{
              "-c:v",gpu_.encoder,"-b:v",std::to_string(video_kbps)+"k","-pix_fmt","yuv420p"});
          else args.insert(args.end(),{"-c:v","libx264","-preset","veryfast","-b:v",
              std::to_string(video_kbps)+"k","-maxrate",std::to_string(video_kbps)+"k",
              "-bufsize",std::to_string(video_kbps*2)+"k","-pix_fmt","yuv420p"});
          args.insert(args.end(),{"-r",number_expr(render_fps)});
          if(direct->include_audio)args.insert(args.end(),{"-c:a","aac","-b:a",
              std::to_string(audio_kbps)+"k","-ar",std::to_string(std::clamp(p.settings.sample_rate,8000,192000))});
          args.insert(args.end(),{"-t",number_expr(total),"-movflags","+faststart",path_to_utf8(encoded.path())});
          ProcessOptions execution;execution.timeout_ms=0;execution.cancel=cancel;
          std::string pending;
          execution.on_output=[&](std::string_view chunk){pending.append(chunk);std::size_t end;
            while((end=pending.find('\n'))!=std::string::npos){
              auto line=pending.substr(0,end);pending.erase(0,end+1);
              if(progress&&line.rfind("out_time_us=",0)==0)try{
                *progress=std::max(progress->load(),std::clamp(std::stod(line.substr(12))/1000000.0/total,0.0,0.99));
              }catch(...){} }
            if(pending.size()>65536)pending.erase(0,pending.size()-65536);
          };
          return run_process(ffmpeg_,args,execution);
        };
        if(options->control)options->control->record("encoder_selected",
            allow_gpu&&gpu_.encode_available?gpu_.encoder:"libx264");
        auto result=encode(allow_gpu&&gpu_.encode_available);
        if((!result.started||result.exit_code!=0)&&!result.cancelled&&
           !(cancel&&cancel->load())&&allow_gpu&&gpu_.encode_available){
          if(options->control)options->control->record("encoder_fallback","Hardware encode failed, retry libx264");
          result=encode(false);
        }
        if(result.cancelled||(cancel&&cancel->load())){if(error)*error="Render cancelled";return false;}
        if(!result.started||result.exit_code!=0){
          if (error) *error = result.output.empty() ? "Direct export failed" : result.output;
          return false;
        }
        if(options->verify_output&&!verify_rendered_video(ffprobe_,ffmpeg_,encoded.path(),
            render_width,render_height,total,cancel,error,direct->include_audio,options->control))return false;
        if(!atomic_replace_file(encoded.path(),destination,error))return false;
        if(progress)*progress=1.0;
        return true;
      }
    }
    TemporaryFile encoded(destination.parent_path(),path_to_utf8(destination.extension()));if(!encoded.valid()){if(error)*error="Could not reserve export file";return false;}
    std::vector<std::string> args{"-nostdin","-hide_banner","-loglevel","warning","-nostats","-progress","pipe:1","-y"};
    struct Input{const Clip* c;const MediaItem* m;int idx;bool generated=false;};std::vector<Input> inputs;std::vector<std::string> temp_text_files;
    struct TextCleanup{std::vector<std::string>& files;~TextCleanup(){for(const auto& file:files){std::error_code ec;fs::remove(path_from_utf8(file),ec);}}}text_cleanup{temp_text_files};int idx=0;
    for(const auto& planned_input:plan.inputs){const auto& c=*planned_input.clip;const auto* m=planned_input.media;
        std::ostringstream start,duration;start<<std::fixed<<std::setprecision(6)<<c.source_offset();duration<<std::fixed<<std::setprecision(6)<<c.duration*c.rate;
        if(m->generated_text.enabled){
            const std::string tmp=temp_name(".pam");temp_text_files.push_back(tmp);if(tmp.empty()||!write_text_pam(tmp,m->generated_text,render_width,render_height)){if(error)*error="Could not rasterize text";return false;}
            args.insert(args.end(),{"-loop","1","-framerate",number_expr(render_fps),"-t",duration.str(),"-i",tmp});inputs.push_back({&c,m,idx++,true});continue;
        }
        if(m->path.empty()||!fs::is_regular_file(path_from_utf8(m->path))){if(error)*error="Missing media: "+m->path;return false;}
        if(is_still_image_path(m->path))args.insert(args.end(),{"-loop","1","-framerate",number_expr(render_fps)});
        else if(c.loop)args.insert(args.end(),{"-stream_loop","-1"});
        args.insert(args.end(),{"-ss",start.str(),"-t",duration.str(),"-i",m->path});inputs.push_back({&c,m,idx++,false});
    }
    const auto cut_segments = environment_utf8("KORALO_EXPORT_FORCE_OVERLAY") == "1"
        ? std::optional<std::vector<CutExportSegment>>{}
        : choose_cut_export(p,plan,render_graph);
    if (options->control && cut_segments)
        options->control->record("render_path","fast_cuts segments=" + std::to_string(cut_segments->size()));
    std::ostringstream fg;
    std::string cur="base0";
    if (cut_segments) {
        // Convert only once per segment, in YUV; no black-frame synthesis,
        // RGBA alpha conversion or N sequential overlay filters.
        for (std::size_t i=0;i<cut_segments->size();++i) {
            const auto& segment=(*cut_segments)[i];
            const auto matched=std::find_if(inputs.begin(),inputs.end(),[&](const Input& in){
                return in.c == segment.clip;
            });
            if(matched==inputs.end()) { if(error)*error="Fast cuts: input missing";return false; }
            fg<<'['<<matched->idx<<":V:0]setpts=PTS-STARTPTS,scale="
              <<render_width<<':'<<render_height<<":force_original_aspect_ratio=decrease,pad="
              <<render_width<<':'<<render_height
              <<":(ow-iw)/2:(oh-ih)/2:black,setsar=1,fps="<<number_expr(render_fps)
              <<",tpad=stop_mode=clone:stop=-1,trim=end_frame="<<segment.frames
              <<",setpts=PTS-STARTPTS,format=yuv420p[cut"<<i<<"];";
        }
        for (std::size_t i=0;i<cut_segments->size();++i) fg<<"[cut"<<i<<']';
        fg<<"concat=n="<<cut_segments->size()<<":v=1:a=0[fastcuts];";
        cur="fastcuts";
    } else {
        fg<<"color=c=black:s="<<render_width<<"x"<<render_height<<":r="<<render_fps<<":d="<<total<<"[base0];";
    }
    int vi=0,ai=0;std::vector<std::string> audioLabels;

    const auto video_layer_for=[&](const Clip* clip)->const RenderGraphVideoLayer*{for(const auto& layer:render_graph.video_layers)if(layer.clip==clip)return &layer;return nullptr;};
    const auto audio_layer_for=[&](const Clip* clip)->const RenderGraphAudioLayer*{for(const auto& layer:render_graph.audio_layers)if(layer.clip==clip)return &layer;return nullptr;};
    const auto auto_duck_expr=[&](const Track* track)->std::string{
        const auto fx=std::find_if(track->audio_effects.begin(),track->audio_effects.end(),[](const EffectInstance& e){return e.enabled&&e.definition_id=="koralo.audio.auto_duck";});if(fx==track->audio_effects.end())return {};
        const auto track_it=std::find_if(p.tracks.begin(),p.tracks.end(),[&](const Track& t){return t.id==track->id;});if(track_it==p.tracks.end())return {};const std::size_t track_index=static_cast<std::size_t>(std::distance(p.tracks.begin(),track_it));
        const double duck=std::pow(10.0,std::clamp(effect_numeric(*fx,"reduction_db",0.0,-12.0),-36.0,0.0)/20.0);const double attack=std::clamp(effect_numeric(*fx,"attack_ms",0.0,120.0),0.0,2000.0)/1000.0,release=std::clamp(effect_numeric(*fx,"release_ms",0.0,350.0),0.0,4000.0)/1000.0;
        const bool audio_solo=std::any_of(p.tracks.begin(),p.tracks.end(),[](const Track& t){return t.kind==TrackKind::Audio&&t.solo;});
        std::vector<std::pair<double,double>> windows;for(const auto& c:p.clips){const auto* other=p.track_by_id(c.track_id);if(!other||other->kind!=TrackKind::Audio||other->mute||c.muted||(audio_solo&&!other->solo))continue;const auto* media=p.media_by_id(c.media_id());if(!media||!media->has_audio)continue;const auto oit=std::find_if(p.tracks.begin(),p.tracks.end(),[&](const Track& t){return t.id==other->id;});if(oit==p.tracks.end()||static_cast<std::size_t>(std::distance(p.tracks.begin(),oit))>=track_index)continue;windows.push_back({std::max(0.0,c.start-attack),c.end()+release});}
        if(windows.empty()) return {};
        std::sort(windows.begin(),windows.end());
        std::vector<std::pair<double,double>> merged;
        for(const auto& w:windows){
            if(merged.empty()||w.first>merged.back().second) merged.push_back(w);
            else merged.back().second=std::max(merged.back().second,w.second);
        }
        std::ostringstream cond;for(std::size_t i=0;i<merged.size();++i){if(i)cond<<'+';cond<<"between(t,"<<number_expr(merged[i].first)<<','<<number_expr(merged[i].second)<<')';}return "if(gt("+cond.str()+",0),"+number_expr(duck)+",1)";
    };
    for(auto& in:inputs){
        if(!cut_segments)if(const auto* layer=video_layer_for(in.c)){auto*t=layer->track;std::string v="v"+std::to_string(vi),n="base"+std::to_string(vi+1);const double alpha=layer->opacity;const double fadeIn=layer->fade_in;const double fadeOut=layer->fade_out;fg<<'['<<in.idx<<":V:0]setpts=PTS-STARTPTS";if(in.generated)fg<<",format=rgba";else fg<<",scale="<<render_width<<":"<<render_height<<":force_original_aspect_ratio=decrease,pad="<<render_width<<":"<<render_height<<":(ow-iw)/2:(oh-ih)/2:black";const std::string local_t="(t/"+number_expr(std::max(0.0001,in.c->rate))+")";if(in.c->video_motion.enabled){const std::string z=animated_expr(in.c->video_motion.zoom,local_t),cx=animated_expr(in.c->video_motion.center_x,local_t),cy=animated_expr(in.c->video_motion.center_y,local_t),rot=animated_expr(in.c->video_motion.rotation,local_t);fg<<",scale=w='trunc("<<render_width<<"*("<<z<<")/2)*2':h='trunc("<<render_height<<"*("<<z<<")/2)*2':eval=frame,crop="<<render_width<<":"<<render_height<<":x='(iw-"<<render_width<<")*("<<cx<<")':y='(ih-"<<render_height<<")*("<<cy<<")',rotate='("<<rot<<")*PI/180':ow=iw:oh=ih:c=black";}append_render_effects(fg,in.m->video_effects,local_t);append_render_effects(fg,in.c->video_effects,local_t);append_render_effects(fg,t->video_effects,local_t);fg<<",format=rgba,colorchannelmixer=aa="<<alpha;if(fadeIn>0.0001)fg<<",fade=t=in:st=0:d="<<fadeIn*std::max(0.0001,in.c->rate)<<":alpha=1";if(fadeOut>0.0001)fg<<",fade=t=out:st="<<std::max(0.0,(in.c->duration-fadeOut)*std::max(0.0001,in.c->rate))<<":d="<<fadeOut*std::max(0.0001,in.c->rate)<<":alpha=1";fg<<",setpts=(PTS-STARTPTS)/"<<std::max(0.0001,in.c->rate)<<"+"<<in.c->start<<"/TB["<<v<<"];["<<cur<<"]["<<v<<"]overlay=0:0:eof_action=pass:enable='between(t,"<<in.c->start<<","<<in.c->end()<<")'["<<n<<"];";cur=n;++vi;}
        if(const auto* layer=audio_layer_for(in.c)){auto*t=layer->track;std::string a="a"+std::to_string(ai++);long delay=static_cast<long>(std::llround(in.c->start*1000));const double fadeIn=layer->fade_in,fadeOut=layer->fade_out;fg<<'['<<in.idx<<":a]asetpts=PTS-STARTPTS,"<<tempo_filter(in.c->rate);if(fadeIn>0.0001)fg<<",afade=t=in:st=0:d="<<fadeIn;if(fadeOut>0.0001)fg<<",afade=t=out:st="<<std::max(0.0,in.c->duration-fadeOut)<<":d="<<fadeOut;if(std::abs(layer->pan)>0.0001)fg<<",aformat=channel_layouts=stereo,stereotools=balance_out="<<layer->pan;append_audio_effects(fg,in.c->audio_effects);append_audio_effects(fg,t->audio_effects);fg<<",adelay="<<delay<<"|"<<delay;const std::string duck=auto_duck_expr(t);fg<<",volume='"<<layer->gain;if(!duck.empty())fg<<"*("<<duck<<")";fg<<"':eval=frame["<<a<<"];";audioLabels.push_back(a);}
    }
    if(!audioLabels.empty()){for(auto&a:audioLabels)fg<<'['<<a<<']';fg<<"amix=inputs="<<audioLabels.size()<<":normalize=0[aout];";}
    if(render_graph.has_output_effects){const std::string outv="voutfx";fg<<'['<<cur<<"]null";append_render_effects(fg,p.video_output_effects,"t");fg<<'['<<outv<<"];";cur=outv;}
    std::string graph=fg.str();if(!graph.empty()&&graph.back()==';')graph.pop_back();
    TemporaryFile filter_file(temporary_directory(),".filters");if(!filter_file.valid()){if(error)*error="Could not create render graph";return false;}
    {std::ofstream script(filter_file.path(),std::ios::binary|std::ios::trunc);script<<graph;script.close();if(!script){if(error)*error="Could not write render graph";return false;}}
    const std::size_t graph_option_index=args.size();
    args.insert(args.end(),{"-/filter_complex",path_to_utf8(filter_file.path()),"-map","["+cur+"]"});
    if(!audioLabels.empty())args.insert(args.end(),{"-map","[aout]"});
    const auto base_args=args;
    auto run_encode=[&](bool hardware){
        auto encode_args=base_args;
        if(legacy_graph_file_option_.load())encode_args[graph_option_index]="-filter_complex_script";
        if(options&&options->lossless_intermediate)encode_args.insert(encode_args.end(),{"-c:v","ffv1","-level","3","-pix_fmt","bgra"});
        else if(hardware&&allow_gpu&&gpu_.encode_available)encode_args.insert(encode_args.end(),{"-c:v",gpu_.encoder,"-b:v",std::to_string(video_kbps)+"k","-pix_fmt","yuv420p"});
        else encode_args.insert(encode_args.end(),{"-c:v","libx264","-preset","veryfast","-b:v",std::to_string(video_kbps)+"k","-maxrate",std::to_string(video_kbps)+"k","-bufsize",std::to_string(video_kbps*2)+"k","-pix_fmt","yuv420p"});
        if(!audioLabels.empty())encode_args.insert(encode_args.end(),{"-c:a","aac","-b:a",std::to_string(audio_kbps)+"k","-ar",std::to_string(std::clamp(p.settings.sample_rate,8000,192000))});
        // Keep the exported file locked to the project/timeline duration. Audio
        // effects such as echo/reverb may otherwise append a filter tail after
        // the last video frame, producing a file slightly longer than the edit.
        encode_args.insert(encode_args.end(),{"-t",number_expr(total),"-movflags","+faststart",path_to_utf8(encoded.path())});
        ProcessOptions execution;execution.timeout_ms=0;execution.cancel=cancel;std::string pending;
        execution.on_output=[&](std::string_view chunk){pending.append(chunk);std::size_t end;
            while((end=pending.find('\n'))!=std::string::npos){auto line=pending.substr(0,end);pending.erase(0,end+1);
                if(progress&&line.rfind("out_time_us=",0)==0)try{*progress=std::max(progress->load(),(options&&options->native_prepared?.6:0.0)+(options&&options->native_prepared?.4:1.0)*std::clamp(std::stod(line.substr(12))/1000000.0/total,0.0,0.99));}catch(...){}
            }if(pending.size()>65536)pending.erase(0,pending.size()-65536);
        };
        auto result=run_process(ffmpeg_,encode_args,execution);
        // The bundled Windows FFmpeg 9 removed -filter_complex_script. Keep
        // graph files (large timelines exceed CreateProcess's command limit),
        // and retry the old spelling only when FFmpeg rejects the new option.
        if(result.started&&result.exit_code!=0&&!result.cancelled&&
           encode_args[graph_option_index]=="-/filter_complex"&&
           (result.output.find("filter_complex")!=std::string::npos||
            result.output.find("Unrecognized")!=std::string::npos||
            result.output.find("not recognized")!=std::string::npos||
            result.output.find("not found")!=std::string::npos)){
            legacy_graph_file_option_=true;
            encode_args[graph_option_index]="-filter_complex_script";
            result=run_process(ffmpeg_,encode_args,execution);
        }
        return result;
    };
    if(options&&options->control)options->control->record("encode_start","FFmpeg graph encode");
    if(progress) *progress=std::max(progress->load(),0.05);
    if(options&&options->control)options->control->record("encoder_selected",
        allow_gpu&&gpu_.encode_available?gpu_.encoder:"libx264");
    auto process=run_encode(allow_gpu&&gpu_.encode_available);
    if((!process.started||process.exit_code!=0)&&!process.cancelled&&!(cancel&&cancel->load())&&allow_gpu&&gpu_.encode_available){
        if(options&&options->control)options->control->record("encoder_fallback","Hardware encode failed, retry libx264");
        process=run_encode(false);
    }
    if(process.cancelled||(cancel&&cancel->load())){if(error)*error="Render cancelled";return false;}
    for(const auto& tmp:temp_text_files){std::error_code ec;fs::remove(path_from_utf8(tmp),ec);}
    if(!process.started||process.exit_code!=0){if(error)*error=process.output.empty()?"render failed":process.output;return false;}
    if(options->verify_output && !verify_rendered_video(ffprobe_,ffmpeg_,encoded.path(),
              render_width,render_height,total,cancel,error,!render_graph.audio_layers.empty(),options->control))return false;
    if(!atomic_replace_file(encoded.path(),destination,error))return false;
    if (progress) {
        *progress = 1.0;
    }
    return true;
}

} // namespace koralo
