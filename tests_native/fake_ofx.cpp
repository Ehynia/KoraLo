#include <cstdint>

#ifdef _WIN32
#define OFX_EXPORT extern "C" __declspec(dllexport)
#else
#define OFX_EXPORT extern "C" __attribute__((visibility("default")))
#endif

using OfxStatus=int;
using OfxPropertySetHandle=void*;
struct OfxHost;
using OfxSetHost=void (*)(OfxHost*);
using OfxMainEntry=OfxStatus (*)(const char*,const void*,OfxPropertySetHandle,OfxPropertySetHandle);
struct OfxPlugin {
    const char* pluginApi;
    int apiVersion;
    const char* pluginIdentifier;
    unsigned int pluginVersionMajor;
    unsigned int pluginVersionMinor;
    OfxSetHost setHost;
    OfxMainEntry mainEntry;
};

static void set_host(OfxHost*){}
static OfxStatus main_entry(const char*,const void*,OfxPropertySetHandle,OfxPropertySetHandle){return 0;}
static OfxPlugin plugin{"OfxImageEffectPluginAPI",1,"com.borisfx.KoraLoFakeContinuum",1,2,&set_host,&main_entry};

OFX_EXPORT int OfxGetNumberOfPlugins(){return 1;}
OFX_EXPORT OfxPlugin* OfxGetPlugin(int index){return index==0?&plugin:nullptr;}
