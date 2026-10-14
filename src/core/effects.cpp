#include "effects.hpp"
#include <cmath>
#include <iomanip>
#include <sstream>
#include <mutex>

namespace koralo {

namespace {
std::mutex g_external_effect_mutex;
std::vector<EffectDefinition> g_external_effect_definitions;

double variant_number(const EffectValue& value,double fallback){
    if(const auto*p=std::get_if<double>(&value))return*p;
    if(const auto*p=std::get_if<int>(&value))return static_cast<double>(*p);
    if(const auto*p=std::get_if<bool>(&value))return *p?1.0:0.0;
    return fallback;
}
}

double EffectParameterState::numeric_value(double time,double fallback) const{
    const double base=variant_number(value,fallback);
    if(!animated||keys.empty())return base;
    if(time<=keys.front().time)return keys.front().value;
    for(std::size_t i=1;i<keys.size();++i){
        const auto&a=keys[i-1];const auto&b=keys[i];
        if(time<=b.time){
            if(a.interpolation==EffectKeyInterpolation::Hold||b.time<=a.time+1e-12)return a.value;
            const double u=std::clamp((time-a.time)/(b.time-a.time),0.0,1.0);
            return a.value+(b.value-a.value)*u;
        }
    }
    return keys.back().value;
}

void EffectParameterState::set_numeric_key(double time,double v,EffectKeyInterpolation interpolation){
    time=std::max(0.0,time);animated=true;
    for(auto&key:keys)if(std::abs(key.time-time)<=1e-6){key={time,v,interpolation};return;}
    keys.push_back({time,v,interpolation});
    std::sort(keys.begin(),keys.end(),[](const EffectKeyframe&a,const EffectKeyframe&b){return a.time<b.time;});
}

const std::vector<EffectDefinition>& builtin_effect_definitions(){
    // Clean-room native catalog. Names mirror common NLE concepts while the
    // implementations are KoraLo-owned and map to our compositor/FFmpeg bridge.
    static const std::vector<EffectDefinition> defs={
        {"koralo.layer_mask","Layer Mask","Layers",EffectMedia|EffectEvent|EffectTrack|EffectOutput,false,{
            {"shape","Shape: 0 Rectangle / 1 Ellipse",EffectParameterType::Double,1.0,0.0,1.0,1.0,false,{}},
            {"center_x","Center X",EffectParameterType::Double,0.5,0.0,1.0,0.01,true,{}},
            {"center_y","Center Y",EffectParameterType::Double,0.5,0.0,1.0,0.01,true,{}},
            {"width","Width",EffectParameterType::Double,0.8,0.01,2.0,0.01,true,{}},
            {"height","Height",EffectParameterType::Double,0.8,0.01,2.0,0.01,true,{}},
            {"feather","Feather",EffectParameterType::Double,0.02,0.0,0.5,0.01,true,{}},
            {"invert","Invert",EffectParameterType::Double,0.0,0.0,1.0,1.0,false,{}}
        }},
        {"koralo.curves","Curves","Color",EffectMedia|EffectEvent|EffectTrack|EffectOutput,false,{
            {"channel","Channel: 0 RGB / 1 R / 2 G / 3 B",EffectParameterType::Double,0.0,0.0,3.0,1.0,false,{}},
            {"black","black",EffectParameterType::Double,0.0,0.0,1.0,0.01,true,{}},
            {"shadows","shadows",EffectParameterType::Double,0.25,0.0,1.0,0.01,true,{}},
            {"midtones","midtones",EffectParameterType::Double,0.5,0.0,1.0,0.01,true,{}},
            {"highlights","highlights",EffectParameterType::Double,0.75,0.0,1.0,0.01,true,{}},
            {"white","white",EffectParameterType::Double,1.0,0.0,1.0,0.01,true,{}}
        }},
        {"koralo.anime_restore_v45","Anime Restore V45 (Spatial)","Anime Restore",EffectMedia|EffectEvent|EffectTrack|EffectOutput,false,{
            {"strength","Strength",EffectParameterType::Double,0.85,0.0,1.5,0.05,true,{}},
            {"threshold","Threshold",EffectParameterType::Double,16.0,0.0,80.0,1.0,true,{}},
            {"line_width","Line Width",EffectParameterType::Double,1.1,0.1,5.0,0.1,true,{}},
            {"edge_hardness","Edge Hardness",EffectParameterType::Double,8.0,1.0,20.0,0.5,true,{}},
            {"color_pull","Color Pull",EffectParameterType::Double,0.85,0.0,1.0,0.05,true,{}},
            {"max_luma","Max Luma",EffectParameterType::Double,230.0,0.0,255.0,1.0,true,{}},
            {"denoise_sigma","Denoise Sigma",EffectParameterType::Double,0.8,0.1,5.0,0.1,true,{}},
            {"local_sigma","Local Sigma",EffectParameterType::Double,3.0,0.1,10.0,0.1,true,{}},
            {"blackhat_size","Blackhat Size",EffectParameterType::Double,7.0,3.0,21.0,2.0,true,{}},
            {"artifact_threshold","Artifact Threshold",EffectParameterType::Double,0.08,0.0,1.0,0.01,true,{}},
            {"saturation","Saturation",EffectParameterType::Double,1.25,0.0,3.0,0.05,true,{}},
            {"contrast","Contrast",EffectParameterType::Double,1.1,0.0,3.0,0.05,true,{}},
            {"flatness","Flatness",EffectParameterType::Double,0.65,0.0,1.0,0.05,true,{}},
            {"levels","Levels",EffectParameterType::Double,48.0,0.0,256.0,1.0,true,{}},
            {"original_blend","Original Blend",EffectParameterType::Double,0.35,0.0,1.0,0.05,true,{}},
            {"restore_text","Restore Text",EffectParameterType::Double,0.0,0.0,1.0,1.0,true,{}},
            {"text_threshold","Text Threshold",EffectParameterType::Double,0.1,0.0,1.0,0.01,true,{}},
            {"text_boost","Text Boost",EffectParameterType::Double,0.2,0.0,1.0,0.05,true,{}},
            {"paper_texture","Paper Texture",EffectParameterType::Double,0.05,0.0,0.5,0.01,true,{}},
            {"paper_warmth","Paper Warmth",EffectParameterType::Double,0.08,0.0,0.5,0.01,true,{}},
            {"paper_scale","Paper Grain Scale",EffectParameterType::Double,1.0,0.25,4.0,0.05,true,{}},
            {"paper_contrast","Paper Grain Contrast",EffectParameterType::Double,1.0,0.0,3.0,0.05,true,{}},
            {"paper_coarse","Paper Coarse Grain",EffectParameterType::Double,0.65,0.0,1.0,0.05,true,{}},
            {"paper_fibers","Paper Fibers",EffectParameterType::Double,0.0,0.0,1.0,0.05,true,{}},
            {"paper_angle","Paper Fiber Angle",EffectParameterType::Double,0.0,-180.0,180.0,5.0,true,{}},
            {"paper_stains","Paper Mottling",EffectParameterType::Double,0.0,0.0,1.0,0.05,true,{}},
            {"paper_aging","Paper Aging",EffectParameterType::Double,0.0,0.0,1.0,0.05,true,{}},
            {"paper_shadows","Paper Shadow Coverage",EffectParameterType::Double,0.0,0.0,1.0,0.05,true,{}},
            {"paper_protect","Paper Edge Protection",EffectParameterType::Double,0.0,0.0,1.0,0.05,true,{}},
            {"paper_seed","Paper Seed",EffectParameterType::Double,0.0,0.0,10000.0,1.0,true,{}},
            {"paper_motion","Paper Animation Rate",EffectParameterType::Double,0.0,0.0,24.0,1.0,true,{}},
            {"post_sharpen","Post Sharpen",EffectParameterType::Double,0.5,0.0,2.0,0.05,true,{}}
        }},
        {"koralo.paint_mask","Paint Mask","Layers",EffectEvent,false,{{"strokes","Strokes",EffectParameterType::String,std::string{},0,0,0,false,{}}}},
        // Native filters derived from the user-provided Anime Line Restore V45
        // control groups.  KoraLo keeps them separate so each stage can be
        // tuned, reordered, bypassed and rendered like any other event FX.
        {"koralo.anime_line_restore","Anime Line Restore","Anime Restore",EffectEvent|EffectTrack|EffectOutput,false,{
            {"strength","Strength",EffectParameterType::Double,0.65,0.0,1.5,0.05,true,{}},
            {"threshold","Detector Threshold",EffectParameterType::Double,22.0,1.0,40.0,1.0,true,{}},
            {"line_width","Line Width",EffectParameterType::Double,0.8,0.5,3.0,0.1,true,{}},
            {"edge_hardness","Edge Hardness",EffectParameterType::Double,8.0,2.0,15.0,0.5,true,{}},
            {"color_pull","Original Line Color",EffectParameterType::Double,0.85,0.0,1.0,0.05,true,{}},
            {"max_luma","Max Luma",EffectParameterType::Double,230.0,80.0,255.0,5.0,false,{}}
        }},
        {"koralo.anime_vector_color","Anime Vector Colors","Anime Restore",EffectEvent|EffectTrack|EffectOutput,false,{
            {"saturation","Saturation",EffectParameterType::Double,1.25,0.0,2.5,0.05,true,{}},
            {"contrast","Contrast",EffectParameterType::Double,1.10,0.5,2.0,0.05,true,{}},
            {"flatness","Fill Smoothing",EffectParameterType::Double,0.65,0.0,1.0,0.05,true,{}},
            {"levels","Color Levels",EffectParameterType::Double,48.0,8.0,128.0,4.0,false,{}},
            {"original_blend","Original Blend",EffectParameterType::Double,0.45,0.0,1.0,0.05,true,{}}
        }},
        {"koralo.anime_text_restore","Anime Text Restore","Anime Restore",EffectEvent|EffectTrack|EffectOutput,false,{
            {"threshold","Text Threshold",EffectParameterType::Double,0.10,0.02,0.35,0.01,true,{}},
            {"boost","Text Boost",EffectParameterType::Double,0.20,0.0,1.0,0.05,true,{}}
        }},
        {"koralo.anime_paper_texture","Anime Paper Texture","Anime Restore",EffectEvent|EffectTrack|EffectOutput,false,{
            {"texture","Texture",EffectParameterType::Double,0.015,0.0,0.5,0.01,true,{}},
            {"warmth","Warmth",EffectParameterType::Double,0.04,0.0,0.5,0.01,true,{}},
            {"paper_scale","Paper Grain Scale",EffectParameterType::Double,1.0,0.25,4.0,0.05,true,{}},
            {"paper_contrast","Paper Grain Contrast",EffectParameterType::Double,1.0,0.0,3.0,0.05,true,{}},
            {"paper_coarse","Paper Coarse Grain",EffectParameterType::Double,0.65,0.0,1.0,0.05,true,{}},
            {"paper_fibers","Paper Fibers",EffectParameterType::Double,0.0,0.0,1.0,0.05,true,{}},
            {"paper_angle","Paper Fiber Angle",EffectParameterType::Double,0.0,-180.0,180.0,5.0,true,{}},
            {"paper_stains","Paper Mottling",EffectParameterType::Double,0.0,0.0,1.0,0.05,true,{}},
            {"paper_aging","Paper Aging",EffectParameterType::Double,0.0,0.0,1.0,0.05,true,{}},
            {"paper_shadows","Paper Shadow Coverage",EffectParameterType::Double,0.0,0.0,1.0,0.05,true,{}},
            {"paper_protect","Paper Edge Protection",EffectParameterType::Double,0.0,0.0,1.0,0.05,true,{}},
            {"paper_seed","Paper Seed",EffectParameterType::Double,0.0,0.0,10000.0,1.0,true,{}},
            {"paper_motion","Paper Animation Rate",EffectParameterType::Double,0.0,0.0,24.0,1.0,true,{}}
        }},
        {"koralo.anime_detail_sharpen","Anime Detail Sharpen","Anime Restore",EffectEvent|EffectTrack|EffectOutput,false,{
            {"amount","Amount",EffectParameterType::Double,0.50,0.0,2.0,0.10,true,{}}
        }},
        {"koralo.anime_deinterlace","Anime Deinterlace","Anime Restore",EffectEvent|EffectTrack|EffectOutput,false,{
            {"strength","Strength",EffectParameterType::Double,1.0,0.0,1.0,0.1,false,{}}
        }},
        // Clean-room native effects selected from common free-editor/filter
        // concepts documented by frei0r/FFmpeg. No third-party GPL code is
        // copied; these are KoraLo-owned implementations with matching generic
        // editing concepts and FFmpeg render equivalents where available.
        {"koralo.glow","Glow","Stylize",EffectEvent|EffectTrack|EffectOutput,false,{
            {"radius","Radius",EffectParameterType::Double,6.0,0.0,30.0,0.5,true,{}},
            {"strength","Strength",EffectParameterType::Double,0.45,0.0,1.5,0.05,true,{}},
            {"threshold","Highlight Threshold",EffectParameterType::Double,0.62,0.0,1.0,0.05,true,{}}
        }},
        {"koralo.hue_shift","Hue Shift","Color",EffectEvent|EffectTrack|EffectOutput,false,{
            {"degrees","Degrees",EffectParameterType::Double,0.0,-180.0,180.0,5.0,true,{}}
        }},
        {"koralo.levels","Levels","Color",EffectEvent|EffectTrack|EffectOutput,false,{
            {"input_black","Input Black",EffectParameterType::Double,0.0,0.0,1.0,0.01,true,{}},
            {"input_white","Input White",EffectParameterType::Double,1.0,0.0,1.0,0.01,true,{}},
            {"gamma","Gamma",EffectParameterType::Double,1.0,0.1,4.0,0.05,true,{}},
            {"output_black","Output Black",EffectParameterType::Double,0.0,0.0,1.0,0.01,true,{}},
            {"output_white","Output White",EffectParameterType::Double,1.0,0.0,1.0,0.01,true,{}}
        }},
        {"koralo.vignette","Vignette","Stylize",EffectEvent|EffectTrack|EffectOutput,false,{
            {"amount","Amount",EffectParameterType::Double,0.45,0.0,1.0,0.05,true,{}},
            {"softness","Softness",EffectParameterType::Double,0.55,0.05,1.0,0.05,true,{}}
        }},
        {"koralo.pixelate","Pixelate / Mosaic","Stylize",EffectEvent|EffectTrack|EffectOutput,false,{
            {"block","Block Size",EffectParameterType::Double,12.0,1.0,96.0,1.0,true,{}}
        }},
        {"koralo.lens_distortion","Lens Distortion","Distort",EffectEvent|EffectTrack|EffectOutput,false,{
            {"k1","Primary",EffectParameterType::Double,0.0,-1.0,1.0,0.05,true,{}},
            {"k2","Secondary",EffectParameterType::Double,0.0,-1.0,1.0,0.05,true,{}},
            {"center_x","Center X",EffectParameterType::Double,0.5,0.0,1.0,0.05,true,{}},
            {"center_y","Center Y",EffectParameterType::Double,0.5,0.0,1.0,0.05,true,{}}
        }},
        {"koralo.rgb_split","RGB Split","Glitch",EffectEvent|EffectTrack|EffectOutput,false,{
            {"horizontal","Horizontal",EffectParameterType::Double,6.0,-64.0,64.0,1.0,true,{}},
            {"vertical","Vertical",EffectParameterType::Double,0.0,-64.0,64.0,1.0,true,{}}
        }},
        {"koralo.letterbox","Letterbox","Stylize",EffectEvent|EffectTrack|EffectOutput,false,{
            {"size","Bar Size",EffectParameterType::Double,0.10,0.0,0.35,0.01,true,{}}
        }},
        {"koralo.scanlines","Scanlines","Stylize",EffectEvent|EffectTrack|EffectOutput,false,{
            {"strength","Strength",EffectParameterType::Double,0.25,0.0,1.0,0.05,true,{}},
            {"spacing","Spacing",EffectParameterType::Double,2.0,2.0,12.0,1.0,false,{}}
        }},
        {"koralo.add_noise","Add Noise","Noise",EffectEvent|EffectTrack|EffectOutput,false,{
            {"amount","Amount",EffectParameterType::Double,8.0,0.0,50.0,1.0,false,{}}
        }},
        {"koralo.black_white","Black and White","Color",EffectEvent|EffectTrack|EffectOutput,false,{
            {"amount","Amount",EffectParameterType::Double,1.0,0.0,1.0,0.05,true,{}}
        }},
        {"koralo.border","Border","Stylize",EffectEvent|EffectTrack|EffectOutput,false,{
            {"thickness","Thickness",EffectParameterType::Double,6.0,0.0,64.0,1.0,false,{}}
        }},
        {"koralo.brightness_contrast","Brightness and Contrast","Color",EffectEvent|EffectTrack|EffectOutput,false,{
            {"brightness","Brightness",EffectParameterType::Double,0.0,-1.0,1.0,0.05,true,{}},
            {"contrast","Contrast",EffectParameterType::Double,0.0,-1.0,1.0,0.05,true,{}}
        }},
        {"koralo.chroma_blur","Chroma Blur","Blur",EffectEvent|EffectTrack|EffectOutput,false,{
            {"radius","Radius",EffectParameterType::Double,2.0,0.0,12.0,0.5,false,{}}
        }},
        {"koralo.chroma_key","Chroma Key","Keying",EffectEvent|EffectTrack|EffectOutput,false,{
            // Clean-room VEGAS-compatible control surface. The supplied VEGAS
            // OFX resources expose color, LowThreshold, HighThreshold,
            // BlurAmount and ShowMaskOnly for the Chroma Keyer. KoraLo uses
            // its own chroma-distance implementation behind these controls.
            {"color_r","Key Red",EffectParameterType::Double,0.0,0.0,1.0,0.01,true,{}},
            {"color_g","Key Green",EffectParameterType::Double,1.0,0.0,1.0,0.01,true,{}},
            {"color_b","Key Blue",EffectParameterType::Double,0.0,0.0,1.0,0.01,true,{}},
            {"low_threshold","Low Threshold",EffectParameterType::Double,0.08,0.0,1.0,0.01,true,{}},
            {"high_threshold","High Threshold",EffectParameterType::Double,0.32,0.0,1.0,0.01,true,{}},
            {"blur_amount","Mask Blur",EffectParameterType::Double,0.0,0.0,1.0,0.01,true,{}},
            {"show_mask_only","Show Mask Only",EffectParameterType::Bool,false,0.0,1.0,1.0,false,{}}
        }},
        {"koralo.color_balance","Color Balance","Color",EffectEvent|EffectTrack|EffectOutput,false,{
            {"red","Red",EffectParameterType::Double,0.0,-1.0,1.0,0.05,false,{}},
            {"green","Green",EffectParameterType::Double,0.0,-1.0,1.0,0.05,false,{}},
            {"blue","Blue",EffectParameterType::Double,0.0,-1.0,1.0,0.05,false,{}}
        }},
        {"koralo.color_corrector","Color Corrector","Color",EffectEvent|EffectTrack|EffectOutput,false,{
            {"brightness","Brightness",EffectParameterType::Double,0.0,-1.0,1.0,0.05,true,{}},
            {"gamma","Gamma",EffectParameterType::Double,1.0,0.1,4.0,0.05,true,{}},
            {"saturation","Saturation",EffectParameterType::Double,1.0,0.0,3.0,0.1,true,{}}
        }},
        {"koralo.defocus","Defocus","Blur",EffectEvent|EffectTrack|EffectOutput,false,{
            {"radius","Radius",EffectParameterType::Double,4.0,0.0,30.0,0.5,true,{}}
        }},
        {"koralo.fill_light","Fill Light","Color",EffectEvent|EffectTrack|EffectOutput,false,{
            {"amount","Amount",EffectParameterType::Double,0.15,0.0,0.75,0.05,true,{}}
        }},
        {"koralo.film_grain","Film Grain","Noise",EffectEvent|EffectTrack|EffectOutput,false,{
            {"amount","Amount",EffectParameterType::Double,10.0,0.0,50.0,1.0,false,{}}
        }},
        {"koralo.gaussian_blur","Gaussian Blur","Blur",EffectEvent|EffectTrack|EffectOutput,false,{
            {"radius","Radius",EffectParameterType::Double,0.0,0.0,20.0,0.5,true,{}}
        }},
        {"koralo.invert","Invert","Color",EffectEvent|EffectTrack|EffectOutput,false,{}},
        {"koralo.saturation","Saturation","Color",EffectEvent|EffectTrack|EffectOutput,false,{
            {"amount","Amount",EffectParameterType::Double,1.0,0.0,3.0,0.1,true,{}}
        }},
        {"koralo.sepia","Sepia","Color",EffectEvent|EffectTrack|EffectOutput,false,{}},
        {"koralo.sharpen","Sharpen","Sharpen",EffectEvent|EffectTrack|EffectOutput,false,{
            {"amount","Amount",EffectParameterType::Double,1.0,0.0,3.0,0.1,false,{}}
        }}
    };
    return defs;
}

const std::vector<EffectDefinition>& builtin_audio_effect_definitions(){
    static const std::vector<EffectDefinition> defs={
        {"koralo.audio.echo","Echo / Delay","Delay",EffectEvent|EffectTrack|EffectOutput,false,{
            {"delay_ms","Delay",EffectParameterType::Double,320.0,20.0,2000.0,10.0,false,{}},
            {"decay","Decay",EffectParameterType::Double,0.38,0.0,0.9,0.02,false,{}},
            {"wet","Wet Mix",EffectParameterType::Double,0.45,0.0,1.0,0.05,false,{}}
        }},
        {"koralo.audio.reverb","Room Reverb","Reverb",EffectEvent|EffectTrack|EffectOutput,false,{
            {"room","Room Size",EffectParameterType::Double,0.55,0.0,1.0,0.05,false,{}},
            {"decay","Decay",EffectParameterType::Double,0.45,0.0,0.9,0.05,false,{}},
            {"wet","Wet Mix",EffectParameterType::Double,0.35,0.0,1.0,0.05,false,{}}
        }},
        {"koralo.audio.chorus","Chorus","Modulation",EffectEvent|EffectTrack|EffectOutput,false,{
            {"depth","Depth",EffectParameterType::Double,0.35,0.0,1.0,0.05,false,{}},
            {"speed","Speed",EffectParameterType::Double,1.2,0.1,5.0,0.1,false,{}},
            {"mix","Mix",EffectParameterType::Double,0.45,0.0,1.0,0.05,false,{}}
        }},
        {"koralo.audio.flanger","Flanger","Modulation",EffectEvent|EffectTrack|EffectOutput,false,{
            {"delay","Delay",EffectParameterType::Double,5.0,0.0,30.0,0.5,false,{}},
            {"depth","Depth",EffectParameterType::Double,3.0,0.0,10.0,0.25,false,{}},
            {"feedback","Feedback",EffectParameterType::Double,0.25,-0.8,0.8,0.05,false,{}},
            {"speed","Speed",EffectParameterType::Double,0.5,0.1,5.0,0.1,false,{}}
        }},
        {"koralo.audio.compressor","Compressor","Dynamics",EffectEvent|EffectTrack|EffectOutput,false,{
            {"threshold_db","Threshold",EffectParameterType::Double,-18.0,-60.0,0.0,1.0,false,{}},
            {"ratio","Ratio",EffectParameterType::Double,4.0,1.0,20.0,0.5,false,{}},
            {"attack_ms","Attack",EffectParameterType::Double,20.0,1.0,200.0,1.0,false,{}},
            {"release_ms","Release",EffectParameterType::Double,250.0,20.0,2000.0,10.0,false,{}},
            {"makeup_db","Makeup",EffectParameterType::Double,2.0,0.0,24.0,0.5,false,{}}
        }},
        {"koralo.audio.limiter","Limiter","Dynamics",EffectEvent|EffectTrack|EffectOutput,false,{
            {"limit","Ceiling",EffectParameterType::Double,0.95,0.1,1.0,0.01,false,{}},
            {"attack_ms","Attack",EffectParameterType::Double,5.0,1.0,80.0,1.0,false,{}},
            {"release_ms","Release",EffectParameterType::Double,50.0,5.0,1000.0,5.0,false,{}}
        }},
        {"koralo.audio.noise_gate","Noise Gate","Dynamics",EffectEvent|EffectTrack|EffectOutput,false,{
            {"threshold_db","Threshold",EffectParameterType::Double,-38.0,-80.0,-2.0,1.0,false,{}},
            {"ratio","Ratio",EffectParameterType::Double,8.0,1.0,20.0,0.5,false,{}},
            {"attack_ms","Attack",EffectParameterType::Double,8.0,1.0,100.0,1.0,false,{}},
            {"release_ms","Release",EffectParameterType::Double,180.0,10.0,1500.0,10.0,false,{}}
        }},
        {"koralo.audio.eq3","3-Band EQ","EQ",EffectEvent|EffectTrack|EffectOutput,false,{
            {"low_db","Low",EffectParameterType::Double,0.0,-18.0,18.0,0.5,false,{}},
            {"mid_db","Mid",EffectParameterType::Double,0.0,-18.0,18.0,0.5,false,{}},
            {"high_db","High",EffectParameterType::Double,0.0,-18.0,18.0,0.5,false,{}}
        }},
        {"koralo.audio.highpass","High Pass","EQ",EffectEvent|EffectTrack|EffectOutput,false,{
            {"frequency","Frequency",EffectParameterType::Double,90.0,20.0,4000.0,10.0,false,{}}
        }},
        {"koralo.audio.lowpass","Low Pass","EQ",EffectEvent|EffectTrack|EffectOutput,false,{
            {"frequency","Frequency",EffectParameterType::Double,16000.0,200.0,20000.0,100.0,false,{}}
        }},
        {"koralo.audio.distortion","Soft Distortion","Drive",EffectEvent|EffectTrack|EffectOutput,false,{
            {"drive_db","Drive",EffectParameterType::Double,8.0,0.0,30.0,0.5,false,{}},
            {"mix","Mix",EffectParameterType::Double,0.6,0.0,1.0,0.05,false,{}}
        }},
        {"koralo.audio.tremolo","Tremolo","Modulation",EffectEvent|EffectTrack|EffectOutput,false,{
            {"frequency","Frequency",EffectParameterType::Double,5.0,0.1,20.0,0.1,false,{}},
            {"depth","Depth",EffectParameterType::Double,0.5,0.0,1.0,0.05,false,{}}
        }},
        {"koralo.audio.stereo_width","Stereo Width","Stereo",EffectEvent|EffectTrack|EffectOutput,false,{
            {"width","Width",EffectParameterType::Double,1.0,0.0,2.0,0.05,false,{}}
        }},
        {"koralo.audio.gain","Gain","Utility",EffectEvent|EffectTrack|EffectOutput,false,{
            {"gain_db","Gain",EffectParameterType::Double,0.0,-48.0,24.0,0.5,false,{}}
        }},
        {"koralo.audio.auto_duck","Auto Duck (track above)","Dynamics",EffectTrack,false,{
            {"reduction_db","Reduction",EffectParameterType::Double,-12.0,-36.0,0.0,1.0,false,{}},
            {"attack_ms","Attack",EffectParameterType::Double,120.0,0.0,2000.0,10.0,false,{}},
            {"release_ms","Release",EffectParameterType::Double,350.0,0.0,4000.0,10.0,false,{}}
        }},
        {"koralo.audio.deesser","De-Esser","Voice",EffectEvent|EffectTrack|EffectOutput,false,{
            {"frequency","Sibilance Frequency",EffectParameterType::Double,6500.0,3000.0,12000.0,100.0,false,{}},
            {"reduction_db","Reduction",EffectParameterType::Double,-6.0,-18.0,0.0,0.5,false,{}}
        }},
        {"koralo.audio.bass_boost","Bass Boost","EQ",EffectEvent|EffectTrack|EffectOutput,false,{
            {"frequency","Frequency",EffectParameterType::Double,110.0,40.0,300.0,5.0,false,{}},
            {"gain_db","Gain",EffectParameterType::Double,5.0,-12.0,18.0,0.5,false,{}}
        }},
        {"koralo.audio.vocal_presence","Vocal Presence","Voice",EffectEvent|EffectTrack|EffectOutput,false,{
            {"frequency","Frequency",EffectParameterType::Double,3200.0,1000.0,7000.0,100.0,false,{}},
            {"gain_db","Gain",EffectParameterType::Double,3.0,-12.0,12.0,0.5,false,{}}
        }},
        {"koralo.audio.normalize","Dynamic Normalize","Dynamics",EffectEvent|EffectTrack|EffectOutput,false,{
            {"strength","Strength",EffectParameterType::Double,0.6,0.0,1.0,0.05,false,{}}
        }}
    };
    return defs;
}

std::vector<const EffectDefinition*> available_audio_effect_definitions(){
    std::vector<const EffectDefinition*> out;
    const auto& defs=builtin_audio_effect_definitions();
    out.reserve(defs.size());
    for(const auto& d:defs)out.push_back(&d);
    return out;
}

const EffectDefinition* audio_effect_definition(const std::string& id){
    const auto& defs=builtin_audio_effect_definitions();
    const auto it=std::find_if(defs.begin(),defs.end(),[&](const auto& d){return d.id==id;});
    return it==defs.end()?nullptr:&*it;
}

EffectInstance make_audio_effect_instance(const std::string& definition_id,int instance_id){
    EffectInstance out;out.id=instance_id;out.definition_id=definition_id;
    if(const auto* def=audio_effect_definition(definition_id))for(const auto& p:def->parameters)out.parameters.push_back({p.id,p.default_value,{},false});
    return out;
}

const std::vector<EffectDefinition>& external_effect_definitions(){ return g_external_effect_definitions; }

void set_external_effect_definitions(std::vector<EffectDefinition> definitions){
    std::lock_guard lock(g_external_effect_mutex);
    g_external_effect_definitions=std::move(definitions);
}

std::vector<const EffectDefinition*> available_effect_definitions(){
    std::vector<const EffectDefinition*> out;
    const auto& builtins=builtin_effect_definitions();
    out.reserve(builtins.size()+g_external_effect_definitions.size());
    for(const auto& d:builtins)out.push_back(&d);
    for(const auto& d:g_external_effect_definitions)out.push_back(&d);
    return out;
}

int effect_preview_cost(const std::vector<EffectInstance>& effects){
    int cost=0;
    for(const auto& e:effects){
        if(!e.enabled)continue;
        if(e.definition_id=="koralo.anime_restore_v45")cost+=15;
        else if(e.definition_id=="koralo.paint_mask")cost+=4;
        else if(e.definition_id=="koralo.anime_line_restore")cost+=5;
        else if(e.definition_id=="koralo.anime_vector_color")cost+=4;
        else if(e.definition_id=="koralo.anime_text_restore")cost+=4;
        else if(e.definition_id=="koralo.gaussian_blur"||e.definition_id=="koralo.defocus"||e.definition_id=="koralo.chroma_blur")cost+=4;
        else if(e.definition_id=="koralo.glow"||e.definition_id=="koralo.lens_distortion"||e.definition_id=="koralo.pixelate")cost+=3;
        else if(e.definition_id=="koralo.rgb_split"||e.definition_id=="koralo.vignette"||e.definition_id=="koralo.scanlines"||e.definition_id=="koralo.letterbox")cost+=2;
        else if(e.definition_id=="koralo.sharpen"||e.definition_id=="koralo.anime_detail_sharpen")cost+=2;
        else if(e.definition_id.rfind("ofx:",0)==0)cost+=8;
        else cost+=1;
    }
    return cost;
}

const EffectDefinition* effect_definition(const std::string&id){
    const auto&defs=builtin_effect_definitions();
    auto it=std::find_if(defs.begin(),defs.end(),[&](const auto&d){return d.id==id;});
    if(it!=defs.end())return &*it;
    {
        std::lock_guard lock(g_external_effect_mutex);
        auto ext=std::find_if(g_external_effect_definitions.begin(),g_external_effect_definitions.end(),[&](const auto&d){return d.id==id;});
        if(ext!=g_external_effect_definitions.end())return &*ext;
    }
    // Imported opaque compatibility nodes are intentionally not part of the
    // user-addable native FX catalog. They preserve chain identity while
    // private VEGAS parameter blobs are still being validated clean-room.
    static const EffectDefinition chroma{"vegas.opaque.chromakeyer","Chroma Keyer (VEGAS import)","Imported / compatibility",EffectEvent,false,{}};
    static const EffectDefinition corrector{"vegas.opaque.colorcorrector","Color Corrector (VEGAS import)","Imported / compatibility",EffectEvent,false,{}};
    static const EffectDefinition balance{"vegas.opaque.colorbalance","Color Balance (VEGAS import)","Imported / compatibility",EffectEvent|EffectMedia,false,{}};
    static const EffectDefinition cookie{"vegas.opaque.cookiecutter","Cookie Cutter (VEGAS import)","Imported / compatibility",EffectEvent,false,{}};
    static const EffectDefinition bezier{"vegas.opaque.bzmasking","VEGAS Bézier Masking (import)","Imported / compatibility",EffectEvent,false,{}};
    static const EffectDefinition secondary{"vegas.opaque.colorcorrectorsecondary","Secondary Color Corrector (VEGAS import)","Imported / compatibility",EffectEvent|EffectMedia,false,{}};
    if(id==chroma.id)return &chroma;
    if(id==corrector.id)return &corrector;
    if(id==balance.id)return &balance;
    if(id==cookie.id)return &cookie;
    if(id==bezier.id)return &bezier;
    if(id==secondary.id)return &secondary;
    return nullptr;
}

EffectInstance make_effect_instance(const std::string&definition_id,int instance_id){
    EffectInstance out;out.id=instance_id;out.definition_id=definition_id;
    if(const auto*def=effect_definition(definition_id)){
        for(const auto&p:def->parameters)out.parameters.push_back({p.id,p.default_value,{},false});
    }
    return out;
}

EffectParameterState* effect_parameter(EffectInstance&instance,const std::string&id){for(auto&p:instance.parameters)if(p.id==id)return&p;return nullptr;}
const EffectParameterState* effect_parameter(const EffectInstance&instance,const std::string&id){for(const auto&p:instance.parameters)if(p.id==id)return&p;return nullptr;}
double effect_numeric(const EffectInstance&instance,const std::string&id,double time,double fallback){const auto*p=effect_parameter(instance,id);return p?p->numeric_value(time,fallback):fallback;}

bool set_effect_numeric(EffectInstance&instance,const std::string&id,double value){
    auto*p=effect_parameter(instance,id);const auto*def=effect_definition(instance.definition_id);if(!def)def=audio_effect_definition(instance.definition_id);if(!p||!def)return false;
    auto it=std::find_if(def->parameters.begin(),def->parameters.end(),[&](const auto&d){return d.id==id;});if(it==def->parameters.end())return false;
    value=std::clamp(value,it->min_value,it->max_value);p->value=value;p->animated=false;p->keys.clear();return true;
}

std::string effect_value_string(const EffectValue&value){
    std::ostringstream os;os<<std::setprecision(12);
    if(const auto* double_value=std::get_if<double>(&value))os<<*double_value;
    else if(const auto* int_value=std::get_if<int>(&value))os<<*int_value;
    else if(const auto* bool_value=std::get_if<bool>(&value))os<<(*bool_value?1:0);
    else if(const auto* string_value=std::get_if<std::string>(&value))os<<*string_value;
    else if(const auto* color_value=std::get_if<EffectColor>(&value))os<<color_value->r<<','<<color_value->g<<','<<color_value->b<<','<<color_value->a;
    else if(const auto* vec_value=std::get_if<EffectVec2>(&value))os<<vec_value->x<<','<<vec_value->y;
    return os.str();
}

} // namespace koralo
