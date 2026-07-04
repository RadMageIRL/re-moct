// PluginLoaderWin.cpp — Windows LoadLibrary implementation of core::IPluginLoader.
//
// Lives in src/platform/win/ (header in include/core/IPluginLoader.h) — the 5th
// seam built INTO the slice-5 core/platform boundary. Compiled only on Windows
// (guarded here + in CMake). The platform::lnx::PosixPluginLoader sibling
// (src/platform/linux/PluginLoaderPosix.cpp) is the dlopen twin.
//
//   load    — LoadLibraryExW on the UTF-8 path (widened via MultiByteToWideChar,
//             self-contained — no StringUtils dependency). LOAD_WITH_ALTERED_
//             SEARCH_PATH so a plugin's co-located dependency DLLs resolve from
//             its own directory when the path is absolute (harmless for a
//             dependency-free plugin like the sine test source).
//   symbol  — GetProcAddress; nullptr if absent.
//   unload  — FreeLibrary in the module dtor.
#ifdef _WIN32

#include "core/IPluginLoader.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstring>

namespace platform::win {

class WinModule final : public core::ILoadedModule {
public:
    explicit WinModule(HMODULE h) : h_(h) {}
    ~WinModule() override { FreeLibrary(h_); }

    void* symbol(const char* name) override {
        // GetProcAddress returns FARPROC (a function pointer). Hand it back as
        // void* via memcpy so no object<->function pointer cast warns; the
        // consumer converts back the same way.
        FARPROC p = GetProcAddress(h_, name);
        void* out = nullptr;
        std::memcpy(&out, &p, sizeof(out));
        return out;
    }

private:
    HMODULE h_;
};

class WinPluginLoader final : public core::IPluginLoader {
public:
    std::unique_ptr<core::ILoadedModule> load(const std::string& path) override {
        int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        if (n <= 0) return nullptr;
        std::wstring w((size_t)(n - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), n);
        HMODULE h = LoadLibraryExW(w.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!h) return nullptr;
        return std::make_unique<WinModule>(h);
    }
};

} // namespace platform::win

namespace core {

// Production default loader (see IPluginLoader.h — reached by name because core
// code can't include this TU's headers). Function-local static: thread-safe
// init; WinPluginLoader is stateless, modules own their HMODULE.
IPluginLoader& pluginLoader() {
    static platform::win::WinPluginLoader instance;
    return instance;
}

} // namespace core

#endif // _WIN32
