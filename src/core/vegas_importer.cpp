#include "vegas_importer.hpp"
#include "../platform/process.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>

namespace koralo {
namespace {
constexpr std::array<unsigned char,16> kRiffWave64 = {
    0x72,0x69,0x66,0x66,0x2e,0x91,0xcf,0x11,0xa5,0xd6,0x28,0xdb,0x04,0xc1,0x00,0x00
};
constexpr std::array<unsigned char,16> kProjectSettingsGuid = {
    0x5a,0x2d,0x8f,0xb2,0x0f,0x23,0xd2,0x11,0x86,0xaf,0x00,0xc0,0x4f,0x8e,0xdb,0x8a
};
constexpr std::array<unsigned char,16> kListGuid = {
    0x6c,0x69,0x73,0x74,0x2f,0x91,0xcf,0x11,0xa5,0xd6,0x28,0xdb,0x04,0xc1,0x00,0x00
};
constexpr std::array<unsigned char,16> kTrackCollectionType = {
    0x47,0x07,0x6c,0x4d,0x16,0x23,0xd2,0x11,0x86,0xb0,0x00,0xc0,0x4f,0x8e,0xdb,0x8a
};
constexpr std::array<unsigned char,16> kTrackType = {
    0x48,0x07,0x6c,0x4d,0x16,0x23,0xd2,0x11,0x86,0xb0,0x00,0xc0,0x4f,0x8e,0xdb,0x8a
};
constexpr std::array<unsigned char,16> kTrackHeaderGuid = {
    0x49,0x07,0x6c,0x4d,0x16,0x23,0xd2,0x11,0x86,0xb0,0x00,0xc0,0x4f,0x8e,0xdb,0x8a
};
constexpr std::array<unsigned char,16> kEventType = {
    0x4a,0x07,0x6c,0x4d,0x16,0x23,0xd2,0x11,0x86,0xb0,0x00,0xc0,0x4f,0x8e,0xdb,0x8a
};
constexpr std::array<unsigned char,16> kEventMetaGuid = {
    0x6a,0x20,0x8d,0x16,0x21,0x23,0xd2,0x11,0x86,0xb0,0x00,0xc0,0x4f,0x8e,0xdb,0x8a
};
constexpr std::array<unsigned char,16> kTakeMetaGuid = {
    0x6d,0x20,0x8d,0x16,0x21,0x23,0xd2,0x11,0x86,0xb0,0x00,0xc0,0x4f,0x8e,0xdb,0x8a
};
constexpr double kVegasTicksPerSecond = 10000000.0;

template<class T> bool read_le(const std::vector<unsigned char>& bytes,std::size_t off,T& out){
    if(off+sizeof(T)>bytes.size())return false;
    std::memcpy(&out,bytes.data()+off,sizeof(T));
    return true;
}

bool guid_equals(const std::vector<unsigned char>& bytes,std::size_t off,const std::array<unsigned char,16>& guid){
    return off+guid.size()<=bytes.size()&&std::equal(guid.begin(),guid.end(),bytes.begin()+static_cast<std::ptrdiff_t>(off));
}

void append_utf8(std::string& out,std::uint32_t cp){
    if(cp<=0x7f)out.push_back(static_cast<char>(cp));
    else if(cp<=0x7ff){out.push_back(static_cast<char>(0xc0|(cp>>6)));out.push_back(static_cast<char>(0x80|(cp&0x3f)));}
    else if(cp<=0xffff){out.push_back(static_cast<char>(0xe0|(cp>>12)));out.push_back(static_cast<char>(0x80|((cp>>6)&0x3f)));out.push_back(static_cast<char>(0x80|(cp&0x3f)));}
    else{out.push_back(static_cast<char>(0xf0|(cp>>18)));out.push_back(static_cast<char>(0x80|((cp>>12)&0x3f)));out.push_back(static_cast<char>(0x80|((cp>>6)&0x3f)));out.push_back(static_cast<char>(0x80|(cp&0x3f)));}
}

std::vector<std::string> utf16_strings(const std::vector<unsigned char>& bytes){
    std::vector<std::string> out;
    auto printable=[](std::uint16_t c){return c>=0x20&&!(c>=0x7f&&c<0xa0);};
    for(std::size_t start=0;start+8<=bytes.size();start+=2){
        // Only begin at the start of a maximal UTF-16LE run.  The old scanner
        // also started at every character inside a path; on some VEGAS 12
        // media-pool records that caused the following sibling path to be
        // skipped.  Maximal runs recover every referenced item deterministically.
        if(start>=2){const std::uint16_t prev=static_cast<std::uint16_t>(bytes[start-2]|(static_cast<unsigned>(bytes[start-1])<<8));if(printable(prev))continue;}
        std::size_t pos=start;std::vector<std::uint16_t> u;
        while(pos+2<=bytes.size()){
            const std::uint16_t c=static_cast<std::uint16_t>(bytes[pos]|(static_cast<unsigned>(bytes[pos+1])<<8));
            if(c==0)break;
            if(!printable(c))break;
            u.push_back(c);pos+=2;if(u.size()>4096)break;
        }
        if(u.size()<4||pos+2>bytes.size()||bytes[pos]!=0||bytes[pos+1]!=0)continue;
        std::string value;
        for(std::size_t i=0;i<u.size();++i){
            std::uint32_t cp=u[i];
            if(cp>=0xd800&&cp<=0xdbff&&i+1<u.size()&&u[i+1]>=0xdc00&&u[i+1]<=0xdfff)cp=0x10000+((cp-0xd800)<<10)+(u[++i]-0xdc00);
            append_utf8(value,cp);
        }
        out.push_back(std::move(value));start=pos;
    }
    return out;
}

std::vector<std::string> utf16_strings_in_range(const std::vector<unsigned char>& bytes,std::size_t start,std::size_t end){
    if(start>=end||end>bytes.size())return{};
    std::vector<unsigned char> part(bytes.begin()+static_cast<std::ptrdiff_t>(start),bytes.begin()+static_cast<std::ptrdiff_t>(end));
    return utf16_strings(part);
}

std::vector<std::string> utf16_windows_paths(const std::vector<unsigned char>& bytes){
    std::vector<std::string> out;
    for(std::size_t start=0;start+8<bytes.size();start+=2){
        if(!std::isalpha(bytes[start])||bytes[start+1]!=0||bytes[start+2]!=':'||bytes[start+3]!=0||bytes[start+4]!='\\'||bytes[start+5]!=0)continue;
        std::size_t pos=start;std::string value;
        while(pos+2<=bytes.size()){
            const std::uint16_t c=static_cast<std::uint16_t>(bytes[pos]|(static_cast<unsigned>(bytes[pos+1])<<8));
            if(c==0)break;
            if(c<0x20||(c>=0x7f&&c<0xa0))break;
            append_utf8(value,c);pos+=2;
        }
        if(value.size()>=4)out.push_back(std::move(value));
        start=pos;
    }
    return out;
}

bool starts_with(const std::string& value,const char* prefix){return value.rfind(prefix,0)==0;}

std::size_t find_utf16_ascii(const std::vector<unsigned char>& bytes,std::size_t begin,std::size_t end,const std::string& text){
    if(begin>=end||end>bytes.size()||text.empty())return std::string::npos;
    std::vector<unsigned char> pattern;pattern.reserve(text.size()*2u+2u);
    for(unsigned char ch:text){pattern.push_back(ch);pattern.push_back(0);}pattern.push_back(0);pattern.push_back(0);
    if(pattern.size()>end-begin)return std::string::npos;
    auto first=bytes.begin()+static_cast<std::ptrdiff_t>(begin),last=bytes.begin()+static_cast<std::ptrdiff_t>(end);
    auto it=std::search(first,last,pattern.begin(),pattern.end());
    if(it==last)return std::string::npos;
    return static_cast<std::size_t>(std::distance(bytes.begin(),it));
}

bool read_named_double(const std::vector<unsigned char>& bytes,std::size_t begin,std::size_t end,const std::string& name,double& value){
    const auto pos=find_utf16_ascii(bytes,begin,end,name);if(pos==std::string::npos)return false;
    const std::size_t off=pos+name.size()*2u+2u;double v=0.0;if(!read_le(bytes,off,v)||!std::isfinite(v))return false;value=v;return true;
}

bool read_named_bool(const std::vector<unsigned char>& bytes,std::size_t begin,std::size_t end,const std::string& name,bool& value){
    const auto pos=find_utf16_ascii(bytes,begin,end,name);if(pos==std::string::npos)return false;
    const std::size_t off=pos+name.size()*2u+2u;if(off>=end)return false;
    // Observed Svfx bool state is stored in the immediate value slot. Treat any
    // non-zero byte in the first 8 bytes as true without assuming the private
    // integer width.
    bool any=false;for(std::size_t i=off;i<std::min(end,off+8u);++i)any|=bytes[i]!=0;value=any;return true;
}

bool read_named_rgb(const std::vector<unsigned char>& bytes,std::size_t begin,std::size_t end,const std::string& name,EffectColor& color){
    const auto pos=find_utf16_ascii(bytes,begin,end,name);if(pos==std::string::npos)return false;
    const std::size_t off=pos+name.size()*2u+2u;double r=0,g=0,b=0;if(!read_le(bytes,off,r)||!read_le(bytes,off+8,g)||!read_le(bytes,off+16,b))return false;
    if(!std::isfinite(r)||!std::isfinite(g)||!std::isfinite(b)||r<0||r>1||g<0||g>1||b<0||b>1)return false;
    color={r,g,b,1.0};
    return true;
}

VegasImportedChromaKey inspect_svfx_chroma_key(const std::vector<unsigned char>& bytes,std::size_t begin,std::size_t end){
    VegasImportedChromaKey out;
    if(find_utf16_ascii(bytes,begin,end,"{Svfx:com.sonycreativesoftware:chromakeyer}")==std::string::npos&&
       find_utf16_ascii(bytes,begin,end,"{Svfx:com.vegascreativesoftware:chromakeyer}")==std::string::npos)return out;
    EffectColor color;double low=0,high=0,blur=0;bool show=false;
    const bool have_color=read_named_rgb(bytes,begin,end,"color",color);
    const bool have_low=read_named_double(bytes,begin,end,"LowThreshold",low);
    const bool have_high=read_named_double(bytes,begin,end,"HighThreshold",high);
    const bool have_blur=read_named_double(bytes,begin,end,"BlurAmount",blur);
    read_named_bool(bytes,begin,end,"ShowMaskOnly",show);
    // The supplied VEGAS 12 project exposes the exact OFX parameter names and
    // immediate scalar/RGB value slots. Accept only plausible normalized values.
    if(have_color&&have_low&&have_high&&have_blur&&low>=0.0&&low<=1.0&&high>=0.0&&high<=1.0&&blur>=0.0&&blur<=1.0){
        out.valid=true;out.color=color;out.low_threshold=low;out.high_threshold=high;out.blur_amount=blur;out.show_mask_only=show;
    }
    return out;
}

bool looks_like_media_path(const std::string& s){
    const std::string base=basename_utf8(s);
    const auto dot=base.find_last_of('.');
    std::string ext=dot==std::string::npos?std::string{}:base.substr(dot);
    std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    static const std::set<std::string> media_ext={
        ".mp4",".mov",".mkv",".avi",".wmv",".asf",".webm",".mxf",".mts",".m2ts",".mpg",".mpeg",
        ".wav",".mp3",".flac",".aac",".m4a",".ogg",".wma",
        ".png",".jpg",".jpeg",".bmp",".tif",".tiff",".webp"
    };
    return media_ext.count(ext)!=0;
}

std::string first_utf16_string(const std::vector<unsigned char>& bytes,std::size_t start,std::size_t end){
    if(start>=end||end>bytes.size())return{};
    std::vector<unsigned char> part(bytes.begin()+static_cast<std::ptrdiff_t>(start),bytes.begin()+static_cast<std::ptrdiff_t>(end));
    auto strings=utf16_strings(part);return strings.empty()?std::string{}:strings.front();
}

struct WaveChunk { std::size_t off=0; std::uint64_t size=0; };
std::vector<WaveChunk> list_children(const std::vector<unsigned char>& bytes,std::size_t list_off){
    std::vector<WaveChunk> out;std::uint64_t list_size=0;if(!read_le(bytes,list_off+16,list_size)||list_size<40)return out;
    const std::size_t end=std::min(bytes.size(),list_off+static_cast<std::size_t>(list_size));std::size_t off=list_off+40;
    while(off+24<=end){std::uint64_t size=0;if(!read_le(bytes,off+16,size)||size<24||off+size>end+7)break;out.push_back({off,size});off=(off+static_cast<std::size_t>(size)+7u)&~std::size_t(7u);}return out;
}

void inspect_tracks(const std::vector<unsigned char>& bytes,VegasImportInspection& result){
    auto add_unique=[](std::vector<std::string>& out,const std::string& value){if(!value.empty()&&std::find(out.begin(),out.end(),value)==out.end())out.push_back(value);};
    for(std::size_t off=40;off+40<=bytes.size();){
        std::uint64_t size=0;if(!read_le(bytes,off+16,size)||size<24||off+size>bytes.size()+7)break;
        if(guid_equals(bytes,off,kListGuid)&&size>=40&&guid_equals(bytes,off+24,kTrackCollectionType)){
            for(const auto& track_chunk:list_children(bytes,off)){
                if(!guid_equals(bytes,track_chunk.off,kListGuid)||track_chunk.size<40||!guid_equals(bytes,track_chunk.off+24,kTrackType))continue;
                VegasImportedTrack track;
                for(const auto& text:utf16_strings_in_range(bytes,track_chunk.off,track_chunk.off+static_cast<std::size_t>(track_chunk.size))){
                    if(starts_with(text,"Sony Track ")){add_unique(track.audio_fx_names,text);add_unique(result.observed_audio_fx_names,text);}
                }
                const auto track_children=list_children(bytes,track_chunk.off);
                for(const auto& child:track_children){
                    if(guid_equals(bytes,child.off,kTrackHeaderGuid)&&child.size>=24+32){std::uint32_t code=0;read_le(bytes,child.off+24+28,code);track.raw_kind=static_cast<int>(code);}
                    if(!guid_equals(bytes,child.off,kListGuid)||child.size<40||!guid_equals(bytes,child.off+24,kTrackHeaderGuid))continue;
                    for(const auto& event_list:list_children(bytes,child.off)){
                        if(!guid_equals(bytes,event_list.off,kListGuid)||event_list.size<40||!guid_equals(bytes,event_list.off+24,kEventType))continue;
                        VegasImportedEvent event;bool have_event=false,have_take=false;
                        for(const auto& event_child:list_children(bytes,event_list.off)){
                            if(guid_equals(bytes,event_child.off,kEventMetaGuid)&&event_child.size>=24+32){
                                std::uint64_t st=0,len=0;read_le(bytes,event_child.off+24+0x10,st);read_le(bytes,event_child.off+24+0x18,len);
                                event.start=st/kVegasTicksPerSecond;event.duration=len/kVegasTicksPerSecond;have_event=std::isfinite(event.start)&&std::isfinite(event.duration)&&event.start>=0.0&&event.duration>0.0&&event.duration<24.0*3600.0;
                            }else if(guid_equals(bytes,event_child.off,kTakeMetaGuid)&&event_child.size>=24+40){
                                std::uint64_t source=0,take_len=0;read_le(bytes,event_child.off+24+0x10,source);read_le(bytes,event_child.off+24+0x18,take_len);
                                event.source_offset=source/kVegasTicksPerSecond;event.media_hint=first_utf16_string(bytes,event_child.off+24+0x28,event_child.off+static_cast<std::size_t>(event_child.size));have_take=std::isfinite(event.source_offset)&&event.source_offset>=0.0&&take_len>0;
                            }
                        }
                        // Event-local plug-in identifiers and generator labels are readable
                        // strings inside the event list. We record identities only; private
                        // parameter blobs remain opaque until independently validated.
                        for(const auto& text:utf16_strings_in_range(bytes,event_list.off,event_list.off+static_cast<std::size_t>(event_list.size))){
                            if(starts_with(text,"{Svfx:")||starts_with(text,"com.vegascreativesoftware:")||starts_with(text,"com.sonycreativesoftware:")){add_unique(event.video_fx_ids,text);add_unique(result.observed_video_fx_ids,text);}
                            if(text.find("(Legacy) Text")!=std::string::npos){event.generated_media=true;add_unique(result.observed_generators,"(Legacy) Text");}
                        }
                        event.chroma_key=inspect_svfx_chroma_key(bytes,event_list.off,event_list.off+static_cast<std::size_t>(event_list.size));
                        if(event.media_hint.find("(Legacy) Text")!=std::string::npos){event.generated_media=true;add_unique(result.observed_generators,"(Legacy) Text");}
                        if(have_event){track.events.push_back(std::move(event));if(!have_take)result.warnings.push_back("A VEGAS event was recovered without a validated Take offset.");}
                    }
                }
                // Preserve empty track slots as well: the supplied VEGAS 12 template has
                // several intentional empty tracks between the generated title and media.
                result.tracks.push_back(std::move(track));
            }
            return;
        }
        off=(off+static_cast<std::size_t>(size)+7u)&~std::size_t(7u);
    }
}
}

std::optional<VegasImportInspection> inspect_vegas_project(const std::string& filename,std::string* error){
    std::ifstream f(path_from_utf8(filename),std::ios::binary);
    if(!f){if(error)*error="Could not open VEGAS project";return std::nullopt;}
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)),{});
    if(bytes.size()<40||!guid_equals(bytes,0,kRiffWave64)){
        if(error)*error="Not a supported VEGAS RIFF/Wave64 project";
        return std::nullopt;
    }
    std::uint64_t declared=0;read_le(bytes,16,declared);
    if(declared<40||declared>bytes.size()+8){if(error)*error="Damaged VEGAS project container";return std::nullopt;}

    VegasImportInspection result;
    // VEGAS 17's project-settings chunk is a stable outer record.  We only
    // consume fields whose offsets and plausibility can be independently
    // validated; unknown bytes remain opaque.
    for(std::size_t off=40;off+24<=bytes.size();){
        std::uint64_t size=0;if(!read_le(bytes,off+16,size)||size<24||off+size>bytes.size()+7)break;
        if(guid_equals(bytes,off,kProjectSettingsGuid)&&size>=24+0x164){
            const std::size_t payload=off+24;
            std::int32_t sample=0;double fps=0.0;
            read_le(bytes,payload+0x0c,sample);read_le(bytes,payload+0x10,fps);
            auto plausible=[&](std::int32_t width,std::int32_t height){return sample>=8000&&sample<=384000&&std::isfinite(fps)&&fps>=1.0&&fps<=1000.0&&width>=16&&width<=32768&&height>=16&&height<=32768;};
            std::int32_t width=0,height=0;
            // Observed layouts: VEGAS 17 and the supplied VEGAS Pro 12 template.
            // Only the independently plausible width/height pair is accepted.
            read_le(bytes,payload+0x15c,width);read_le(bytes,payload+0x160,height);
            if(!plausible(width,height)&&size>=24+0x532){read_le(bytes,payload+0x52a,width);read_le(bytes,payload+0x52e,height);}
            if(plausible(width,height)){
                result.settings.sample_rate=sample;result.settings.fps=fps;result.settings.width=width;result.settings.height=height;result.has_settings=true;
            }else result.warnings.push_back("Project settings record was present but failed all validated layout plausibility checks.");
            break;
        }
        off=(off+static_cast<std::size_t>(size)+7u)&~std::size_t(7u);
    }

    std::set<std::string> seen;
    auto collect_media=[&](const std::vector<std::string>& strings){for(const auto& s:strings){
        if(!looks_like_media_path(s))continue;
        const std::string base=basename_utf8(s);const auto dot=base.find_last_of('.');std::string ext=dot==std::string::npos?std::string{}:base.substr(dot);
        std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
        if(ext==".veg"||ext==".kora")continue;
        if(seen.insert(s).second)result.media_paths.push_back(s);
    }};
    collect_media(utf16_strings(bytes));
    collect_media(utf16_windows_paths(bytes));
    if(result.media_paths.empty())result.warnings.push_back("No referenced media paths were recovered from this VEG project.");
    inspect_tracks(bytes,result);
    bool any_events=false;for(const auto& track:result.tracks)if(!track.events.empty()){any_events=true;break;}
    if(!any_events)result.warnings.push_back("No validated TrackEvent timing records were recovered from this VEG project.");
    else result.warnings.push_back("Experimental TrackEvent timing import recovered track order, empty track slots, start/length/Take offset and event FX identities. The old Svfx Chroma Keyer color/threshold/blur/mask fields are decoded when their validated named value slots are present; other FX blobs, fades, masks and groups remain opaque until separately validated.");
    return result;
}

Project make_vegas_import_project(const VegasImportInspection& inspection,const std::string& source_filename){
    Project p=Project::make_default();
    if(inspection.has_settings)p.settings=inspection.settings;
    auto stem=path_from_utf8(source_filename).stem();
    p.name=path_to_utf8(stem)+".kora";
    p.path.clear();
    return p;
}

} // namespace koralo
