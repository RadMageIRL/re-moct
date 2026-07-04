// IPluginLoader.h — platform-neutral shared-library load seam (Phase 4, slice a).
//
// `core::IPluginLoader` is the interface; `platform::win::WinPluginLoader`
// (src/platform/win/PluginLoaderWin.cpp, LoadLibrary) and
// `platform::lnx::PosixPluginLoader` (src/platform/linux/PluginLoaderPosix.cpp,
// dlopen) are the siblings — the 5th platform seam, built INTO the slice-5
// core/platform boundary exactly like IHttp/IIpc/INotify/ICdIo.
//
// The abstraction is the thinnest platform primitive the ONE real consumer (the
// plugin host, core::loadPlugin in PluginHost.h) needs: "load a shared library
// by path -> a module object; resolve a symbol by name; unload on destroy." All
// POLICY stays consumer-side — the remoct_plugin_query resolution, the ABI
// version gate, and descriptor validation live in PluginHost (the same
// transport/protocol split as every seam: IIpc carries bytes, DiscordRP owns the
// framing). Only the raw OS calls (LoadLibraryExW/GetProcAddress/FreeLibrary vs
// dlopen/dlsym/dlclose) live behind this seam.
//
// This header must NOT include <windows.h> or any platform header — the same
// "does core stay portable" test as the Phase 1 seams.
#pragma once
#include <memory>
#include <string>

namespace core {

// One loaded shared module (a plugin .dll/.so). Destroying it UNLOADS the module
// (FreeLibrary/dlclose) — so any pointer resolved from it (including the
// RemoctPlugin descriptor) is valid only for this object's lifetime. The host
// keeps the module alive for as long as it holds the descriptor.
class ILoadedModule {
public:
    virtual ~ILoadedModule() = default;
    // Resolve an exported symbol by name; nullptr if absent. Returned as void*
    // (GetProcAddress/dlsym shape); the caller converts to the function type
    // (via memcpy — the warning-free object<->function pointer bridge).
    virtual void* symbol(const char* name) = 0;
};

// Factory. load() takes a filesystem path (UTF-8 on Windows; widened by the
// impl). Returns nullptr when the module can't be loaded (not found / not a
// loadable image / a missing dependency under RTLD_NOW). Probing/validation is
// the CONSUMER's job, not the seam's.
class IPluginLoader {
public:
    virtual ~IPluginLoader() = default;
    virtual std::unique_ptr<ILoadedModule> load(const std::string& path) = 0;
};

// Link-time bridge to the platform impl (defined in the impl TU): core code
// cannot #include a platform header, so the production default is reached by
// name — the core::ipc()/notifier()/cdio() precedent. The single consumer
// (core::loadPlugin) takes an IPluginLoader* by injection (tests pass a fake);
// this accessor only supplies its production default. No setPluginLoader().
IPluginLoader& pluginLoader();

} // namespace core
