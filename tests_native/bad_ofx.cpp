#include <cstdlib>

#if defined(_WIN32)
#define OFX_EXPORT extern "C" __declspec(dllexport)
#else
#define OFX_EXPORT extern "C" __attribute__((visibility("default")))
#endif

struct OfxPlugin;

OFX_EXPORT int OfxGetNumberOfPlugins() {
    std::abort();
}

OFX_EXPORT OfxPlugin* OfxGetPlugin(int) {
    return nullptr;
}
