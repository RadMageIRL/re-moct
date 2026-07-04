// PluginLoaderPosix.cpp — Linux dlopen implementation of core::IPluginLoader
// (Phase 4, slice a). The platform::win::WinPluginLoader (LoadLibrary) sibling.
//
//   load    — dlopen(RTLD_NOW | RTLD_LOCAL). RTLD_NOW resolves every symbol up
//             front (fail fast on a missing host symbol, rather than a lazy
//             SIGSEGV mid-playback); RTLD_LOCAL keeps a plugin's symbols out of
//             the global namespace so plugins can't collide with each other or
//             the host. nullptr on failure (dlerror carries why; the consumer
//             just sees "couldn't load").
//   symbol  — dlsym; nullptr if absent. Returned as void* via memcpy so no
//             object<->function pointer cast trips -Wpedantic (the
//             remoct_linux_seams OBJECT target builds this with -Wall -Wextra
//             -Wpedantic). POSIX guarantees the value is convertible to a
//             function pointer.
//   unload  — dlclose in the module dtor.
#if defined(__linux__)

#include "core/IPluginLoader.h"

#include <dlfcn.h>
#include <cstring>

namespace platform::lnx {

class PosixModule final : public core::ILoadedModule {
public:
    explicit PosixModule(void* h) : h_(h) {}
    ~PosixModule() override { dlclose(h_); }

    void* symbol(const char* name) override {
        void* p = dlsym(h_, name);   // already a void*; kept explicit for parity
        void* out = nullptr;
        std::memcpy(&out, &p, sizeof(out));
        return out;
    }

private:
    void* h_;
};

class PosixPluginLoader final : public core::IPluginLoader {
public:
    std::unique_ptr<core::ILoadedModule> load(const std::string& path) override {
        void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) return nullptr;
        return std::make_unique<PosixModule>(h);
    }
};

} // namespace platform::lnx

namespace core {

// Production default loader (see IPluginLoader.h — reached by name because core
// code can't include this TU's headers). Function-local static: thread-safe
// init; PosixPluginLoader is stateless, modules own their dl handle.
IPluginLoader& pluginLoader() {
    static platform::lnx::PosixPluginLoader instance;
    return instance;
}

} // namespace core

#endif // __linux__
