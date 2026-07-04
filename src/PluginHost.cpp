// PluginHost.cpp — the platform-neutral plugin host policy (Phase 4, slice a).
// See PluginHost.h. No platform header — compiles on both matrix jobs.
#include "PluginHost.h"

#include <cstddef>   // offsetof
#include <cstring>   // memcpy (void* -> function pointer, warning-free)

namespace core {

const char* pluginLoadName(PluginLoad r) {
    switch (r) {
        case PluginLoad::Ok:               return "ok";
        case PluginLoad::ModuleLoadFailed: return "module load failed";
        case PluginLoad::EntryMissing:     return "remoct_plugin_query missing";
        case PluginLoad::NullDescriptor:   return "null descriptor";
        case PluginLoad::AbiMismatch:      return "ABI version mismatch";
        case PluginLoad::TooSmall:         return "descriptor too small";
        case PluginLoad::MissingRequiredFn:return "missing required function";
    }
    return "unknown";
}

// The REQUIRED function pointers a v1 plugin must provide — without any of these
// the source is undriveable. Everything after `close` (metadata/config) is
// OPTIONAL and host-null-checked (remoct_plugin.h). See PluginHost.h.
static bool hasRequiredFns(const RemoctPlugin* d) {
    return d->create && d->destroy && d->open && d->read_frames && d->close;
}

PluginLoad validatePlugin(const RemoctPlugin* d, uint32_t host_abi) {
    if (!d) return PluginLoad::NullDescriptor;
    // MAJOR gate: refuse an incompatible ABI before anything else touches it.
    if (d->abi_version != host_abi) return PluginLoad::AbiMismatch;
    // struct_size must physically reach through the last REQUIRED field (close),
    // so the host can safely read every required pointer slot without reading
    // past the plugin's own struct. Optional trailing fields (metadata/config)
    // are gated separately at call time (the additive-growth contract).
    if (d->struct_size < offsetof(RemoctPlugin, close) + sizeof(void (*)()))
        return PluginLoad::TooSmall;
    if (!hasRequiredFns(d)) return PluginLoad::MissingRequiredFn;
    return PluginLoad::Ok;
}

std::unique_ptr<LoadedPlugin> loadPlugin(const std::string& path,
                                         PluginLoad* out, IPluginLoader* loader) {
    IPluginLoader& L = loader ? *loader : pluginLoader();

    std::unique_ptr<ILoadedModule> mod = L.load(path);
    if (!mod) { if (out) *out = PluginLoad::ModuleLoadFailed; return nullptr; }

    // Resolve the one exported entry. void* -> function pointer via memcpy (the
    // warning-free bridge; -Wpedantic forbids the direct cast).
    void* sym = mod->symbol("remoct_plugin_query");
    if (!sym) { if (out) *out = PluginLoad::EntryMissing; return nullptr; }
    RemoctQueryFn query = nullptr;
    std::memcpy(&query, &sym, sizeof(query));

    const RemoctPlugin* d = query();
    PluginLoad v = validatePlugin(d, REMOCT_ABI_VERSION);
    if (v != PluginLoad::Ok) { if (out) *out = v; return nullptr; }

    if (out) *out = PluginLoad::Ok;
    return std::make_unique<LoadedPlugin>(std::move(mod), d);
}

} // namespace core
