// HttpWinInet.cpp — Windows/WinINet implementation of core::IHttp.
//
// Lives in src/ per project convention (header in include/IHttp.h); moves into
// src/platform/win/ at the later core/platform reorg. Compiled only on Windows
// (guarded in CMakeLists via if(WIN32)).
//
// Two request shapes, each mirroring the per-module helpers it replaces:
//   GET  (slice 1)  — InternetOpenUrlW: MBLookup, RadioBrowser, and the auth GETs.
//   POST (slice 2)  — InternetConnectW + HttpOpenRequestW + HttpSendRequestW with a
//                     request body: the LastFm / ListenBrainz scrobble writes.
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

// Extra request headers (UTF-8 -> wide, CRLF-joined). Content-Type is emitted first
// when set, then the caller's headers (e.g. Authorization) — matching the byte order
// the scrobble helpers wrote ("Content-Type: ...\r\nAuthorization: ...\r\n").
std::wstring buildHeaders(const core::HttpRequest& req) {
    std::wstring h;
    if (!req.content_type.empty()) {
        h += L"Content-Type: "; h += toWideUtf8(req.content_type); h += L"\r\n";
    }
    for (const auto& kv : req.headers) {
        h += toWideUtf8(kv.first);  h += L": ";
        h += toWideUtf8(kv.second); h += L"\r\n";
    }
    return h;
}

long queryStatus(HINTERNET h) {
    DWORD code = 0, len = sizeof(code), idx = 0;
    if (HttpQueryInfoW(h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &len, &idx))
        return (long)code;
    return 0;
}

std::string queryFinalUrl(HINTERNET h) {
    wchar_t fu[INTERNET_MAX_URL_LENGTH];
    DWORD ful = INTERNET_MAX_URL_LENGTH;
    if (InternetQueryOptionW(h, INTERNET_OPTION_URL, fu, &ful))
        return toUtf8(fu, (int)wcslen(fu));
    return {};
}

void readBody(HINTERNET h, const core::HttpRequest& req, core::HttpResponse& res) {
    char buf[4096];
    DWORD bytes = 0;
    while (InternetReadFile(h, buf, sizeof(buf), &bytes)) {
        if (bytes == 0) break;   // clean EOF
        res.body.append(buf, bytes);
        if (req.max_body && res.body.size() > req.max_body) {
            res.truncated = true;   // matches the old ">cap -> break" behavior
            break;
        }
    }
}

// The app's canonical UA, matching the string every migrated helper used.
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

        const bool is_get = req.method.empty() || req.method == "GET";

        if (req.timeout_ms > 0) {
            DWORD to = (DWORD)req.timeout_ms;
            InternetSetOptionW(inet, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
            InternetSetOptionW(inet, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));
            // SEND timeout: set on the GET path only. That preserves slice 1's
            // behavior (RadioBrowser's baseline set all three). The POST path does
            // NOT set it — the scrobble helpers it replaces set only CONNECT+RECEIVE,
            // and a request-body send never blocks on these tiny payloads anyway.
            if (is_get)
                InternetSetOptionW(inet, INTERNET_OPTION_SEND_TIMEOUT, &to, sizeof(to));
        }

        DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE;
        if (core::urlIsSecureScheme(req.url)) flags |= INTERNET_FLAG_SECURE;
        switch (req.redirect) {
            case core::RedirectPolicy::FollowAll:  break;   // no flag: WinINet default follow
            case core::RedirectPolicy::FollowNone: flags |= INTERNET_FLAG_NO_AUTO_REDIRECT; break;
            case core::RedirectPolicy::FollowSameScheme:
                flags |= INTERNET_FLAG_IGNORE_REDIRECT_TO_HTTP |
                         INTERNET_FLAG_IGNORE_REDIRECT_TO_HTTPS; break;
        }

        std::wstring hdrs = buildHeaders(req);
        const wchar_t* hp = hdrs.empty() ? nullptr : hdrs.c_str();
        DWORD hlen = hdrs.empty() ? 0 : (DWORD)-1L;

        if (is_get) {
            // ── GET: one-shot InternetOpenUrlW (unchanged since slice 1) ──
            std::wstring wurl = toWideUtf8(req.url);
            HINTERNET conn = InternetOpenUrlW(inet, wurl.c_str(), hp, hlen, flags, 0);
            if (!conn) { InternetCloseHandle(inet); return res; }
            res.status    = queryStatus(conn);
            readBody(conn, req, res);
            res.final_url = queryFinalUrl(conn);
            core::finalizeBody(res, req);   // clears body on reject_truncated + truncated
            InternetCloseHandle(conn);
        } else {
            // ── non-GET (POST/…): InternetConnect + HttpOpenRequest + body ──
            std::wstring wurl = toWideUtf8(req.url);
            URL_COMPONENTSW uc{};
            wchar_t host[256] = {}, path[4096] = {}, extra[4096] = {};
            uc.dwStructSize     = sizeof(uc);
            uc.lpszHostName     = host;  uc.dwHostNameLength = 256;
            uc.lpszUrlPath      = path;  uc.dwUrlPathLength  = 4096;
            uc.lpszExtraInfo    = extra; uc.dwExtraInfoLength = 4096;
            if (!InternetCrackUrlW(wurl.c_str(), (DWORD)wurl.size(), 0, &uc)) {
                InternetCloseHandle(inet); return res;
            }
            std::wstring full_path = std::wstring(path) + extra;   // path + query

            HINTERNET conn = InternetConnectW(inet, host, uc.nPort, nullptr, nullptr,
                                              INTERNET_SERVICE_HTTP, 0, 0);
            if (!conn) { InternetCloseHandle(inet); return res; }

            std::wstring wverb = toWideUtf8(req.method);
            HINTERNET reqh = HttpOpenRequestW(conn, wverb.c_str(), full_path.c_str(),
                                              nullptr, nullptr, nullptr, flags, 0);
            if (!reqh) { InternetCloseHandle(conn); InternetCloseHandle(inet); return res; }

            // Body is sent as RAW bytes — never widened. nlohmann emits raw UTF-8,
            // and HttpSendRequest's payload is a byte buffer; widening would corrupt
            // a non-ASCII scrobble ("Björk", "Sigur Rós", …).
            if (HttpSendRequestW(reqh, hp, hlen,
                                 (LPVOID)req.body.data(), (DWORD)req.body.size())) {
                res.status    = queryStatus(reqh);
                readBody(reqh, req, res);
                res.final_url = queryFinalUrl(reqh);
                core::finalizeBody(res, req);
            }
            InternetCloseHandle(reqh);
            InternetCloseHandle(conn);
        }

        InternetCloseHandle(inet);
        return res;
    }
};

} // namespace platform::win

namespace core {

// Transitional injection pointer (see IHttp.h). nullptr in production -> the WinINet
// singleton; a test may point it at a fake transport via setHttp().
static IHttp* g_override = nullptr;

void setHttp(IHttp* transport) { g_override = transport; }

// Function-local static -> thread-safe init; WinInetHttp is stateless, so concurrent
// fetch() calls from different threads each open their own handles.
IHttp& http() {
    if (g_override) return *g_override;
    static platform::win::WinInetHttp instance;
    return instance;
}

} // namespace core

#endif // _WIN32
