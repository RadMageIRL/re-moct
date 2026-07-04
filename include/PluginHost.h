// PluginHost.h — the platform-neutral plugin host policy (Phase 4, slice a).
//
// The consumer of the core::IPluginLoader seam. Where the seam does the raw OS
// load, THIS is the policy the seam deliberately doesn't carry: resolve the one
// exported entry (remoct_plugin_query), run the ABI version gate + descriptor
// validation, and hold the module alive for as long as the borrowed descriptor
// is used. Platform-free (no windows.h) — it only talks to the seam interface
// and the C ABI header, so it compiles on Linux CI unchanged.
//
// Slice (a) proves this against the disposable sine plugin; slice (b) is where
// AudioManager drives a real source through the returned RemoctPlugin table.
#pragma once
#include <memory>
#include <string>

#include "core/remoct_plugin.h"
#include "core/IPluginLoader.h"

namespace core {

// Why a plugin load succeeded or failed. The three "reject" cases the version
// gate exists for (NullDescriptor/AbiMismatch/TooSmall) plus MissingRequiredFn
// are what a host logs and refuses on.
enum class PluginLoad {
    Ok,
    ModuleLoadFailed,   // the .so/.dll couldn't be loaded (missing / not an image)
    EntryMissing,       // remoct_plugin_query not exported
    NullDescriptor,     // the entry returned nullptr
    AbiMismatch,        // abi_version != host REMOCT_ABI_VERSION (the major gate)
    TooSmall,           // struct_size doesn't reach through the required fields
    MissingRequiredFn,  // a REQUIRED function pointer (create/destroy/open/
                        // read_frames/close) is null
};

// Human-readable reason, for the host's refusal log line.
const char* pluginLoadName(PluginLoad r);

// Pure descriptor validation (no I/O): the major version gate + the shape check.
// Testable with hand-built descriptors (the reject-path coverage). host_abi is
// normally REMOCT_ABI_VERSION. Returns PluginLoad::Ok iff the descriptor is a
// usable v1 (compatible) plugin.
PluginLoad validatePlugin(const RemoctPlugin* d, uint32_t host_abi);

// A loaded, validated plugin: owns the module (unloads on destroy) AND holds the
// borrowed descriptor pointer (which lives inside the module, so the module must
// outlive it — enforced by member order). Non-copyable. The descriptor's
// function pointers are the plugin's ops the host calls.
class LoadedPlugin {
public:
    LoadedPlugin(std::unique_ptr<ILoadedModule> mod, const RemoctPlugin* desc)
        : mod_(std::move(mod)), desc_(desc) {}
    LoadedPlugin(const LoadedPlugin&)            = delete;
    LoadedPlugin& operator=(const LoadedPlugin&) = delete;

    const RemoctPlugin* plugin() const { return desc_; }

private:
    // Destruction is reverse declaration order: desc_ (a trivial borrowed
    // pointer) first, then mod_ (the FreeLibrary/dlclose) LAST — so the module
    // is never unloaded while the descriptor could still be dereferenced.
    std::unique_ptr<ILoadedModule> mod_;
    const RemoctPlugin*            desc_;
};

// Load + validate a plugin from a filesystem path. On success returns the holder
// and sets *out = Ok; on failure returns nullptr and sets *out to the reason
// (out may be null if the caller doesn't care). `loader` defaults to the
// platform seam (core::pluginLoader()); injectable so a test can supply a fake.
std::unique_ptr<LoadedPlugin> loadPlugin(const std::string& path,
                                         PluginLoad* out = nullptr,
                                         IPluginLoader* loader = nullptr);

} // namespace core
