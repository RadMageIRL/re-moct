// HttpWinInet.cpp — Windows/WinINet implementation of core::IHttp.
//
// Lives in src/ per project convention (header in include/IHttp.h); moves into
// src/platform/win/ at the later core/platform reorg. Compiled only on Windows
// (guarded in CMakeLists via if(WIN32)).
//
// Slice 1 behavior mirrors the per-call GET helpers it replaces (MBLookup,
// RadioBrowser): per-call InternetOpenW + InternetOpenUrlW + read-to-EOF + close,
// PRECONFIG proxy, RELOAD|NO_CACHE_WRITE, INTERNET_FLAG_SECURE derived from the URL
// scheme. No persistent session, no streaming — the live audio read loop in
// StreamSource stays out of this seam by design.
#ifdef _WIN32

#include "IHttp.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wininet.h>

namespace {

std::wstring toWideUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string toUtf8(const wchar_t* w, int wlen) {
    if (wlen <= 0) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, wlen, nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, wlen, s.data(), n, nullptr, nullptr);
    return s;
}

bool isHttps(const std::string& url) {
    return url.size() >= 8 &&
           (url.compare(0, 8, "https://") == 0 || url.compare(0, 8, "HTTPS://") == 0);
}

// The app's canonical UA, matching the string the migrated GET helpers used.
constexpr const wchar_t* kDefaultUA =
    L"RE-MOCT/1.0.0-rc1 (https://github.com/RadMageIRL/re-moct)";

} // namespace

namespace platform::win {

struct WinInetHttp final : core::IHttp {
    core::HttpResponse fetch(const core::HttpRequest& req) override {
        core::HttpResponse res;

        std::wstring wua = req.user_agent.empty() ? std::wstring(kDefaultUA)
                                                  : toWideUtf8(req.user_agent);
        HINTERNET inet = InternetOpenW(wua.c_str(), INTERNET_OPEN_TYPE_PRECONFIG,
                                       nullptr, nullptr, 0);
        if (!inet) return res;   // ok stays false

        if (req.timeout_ms > 0) {
            DWORD to = (DWORD)req.timeout_ms;
            InternetSetOptionW(inet, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
            InternetSetOptionW(inet, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));
            InternetSetOptionW(inet, INTERNET_OPTION_SEND_TIMEOUT,    &to, sizeof(to));
        }

        DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE;
        if (isHttps(req.url))         flags |= INTERNET_FLAG_SECURE;
        if (!req.follow_redirects)    flags |= INTERNET_FLAG_NO_AUTO_REDIRECT;

        // Optional extra request headers (UTF-8 -> wide, CRLF-joined).
        std::wstring hdrs;
        for (const auto& h : req.headers) {
            hdrs += toWideUtf8(h.first);  hdrs += L": ";
            hdrs += toWideUtf8(h.second); hdrs += L"\r\n";
        }
        const wchar_t* hp = hdrs.empty() ? nullptr : hdrs.c_str();
        DWORD hlen = hdrs.empty() ? 0 : (DWORD)-1L;

        std::wstring wurl = toWideUtf8(req.url);
        HINTERNET conn = InternetOpenUrlW(inet, wurl.c_str(), hp, hlen, flags, 0);
        if (!conn) { InternetCloseHandle(inet); return res; }

        // HTTP status (surfaced even though slice-1 consumers ignore it).
        DWORD code = 0, clen = sizeof(code), idx = 0;
        if (HttpQueryInfoW(conn, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                           &code, &clen, &idx))
            res.status = (long)code;

        char buf[4096];
        DWORD bytes = 0;
        while (InternetReadFile(conn, buf, sizeof(buf), &bytes)) {
            if (bytes == 0) break;   // clean EOF
            res.body.append(buf, bytes);
            if (req.max_body && res.body.size() > req.max_body) {
                res.truncated = true;   // matches the old ">cap -> break" behavior
                break;
            }
        }

        // Final (post-redirect) URL — cheap; used by a later site for token URLs.
        wchar_t fu[INTERNET_MAX_URL_LENGTH];
        DWORD ful = INTERNET_MAX_URL_LENGTH;
        if (InternetQueryOptionW(conn, INTERNET_OPTION_URL, fu, &ful))
            res.final_url = toUtf8(fu, (int)wcslen(fu));

        InternetCloseHandle(conn);
        InternetCloseHandle(inet);

        // Transport-level success: connected and read. A truncated body is still a
        // success unless the caller asked to reject it (CoverArt's 10 MB semantics,
        // used in a later slice). Slice-1 callers pass reject_truncated=false, so a
        // capped body is returned as-is — identical to the old helpers.
        res.ok = req.reject_truncated ? !res.truncated : true;
        return res;
    }
};

} // namespace platform::win

namespace core {
// Transitional singleton (see IHttp.h). Function-local static -> thread-safe init;
// WinInetHttp is stateless, so concurrent fetch() calls from different threads each
// open their own handles and do not share mutable state.
IHttp& http() {
    static platform::win::WinInetHttp instance;
    return instance;
}
} // namespace core

#endif // _WIN32
