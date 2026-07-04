#include "IHeartIdentity.h"

#include "PortUtil.h"     // port::tickMs (rand seed) — no WinINet
#include "json.hpp"

#include <fstream>
#include <cstdlib>
#include <cstdio>
#include <cctype>
#include <filesystem>
#include <system_error>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

// Persisted-identity location. Windows: %APPDATA%\RE-MOCT\ (the app-data root the
// deep log's logsDir() sits under, minus the \logs subdir — the identity is config,
// not a capture). Linux: $XDG_STATE_HOME/re-moct/ (fallback ~/.local/state/re-moct/),
// alongside the deep-log parent. Returns "" if no base dir is resolvable.
std::string identityDir() {
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    if (!appdata || !*appdata) return std::string();
    std::string base = std::string(appdata) + "\\RE-MOCT";
    std::error_code ec;
    fs::create_directories(fs::path(base), ec);   // ok if it already exists
    if (ec) return std::string();
    return base + "\\";
#else
    std::string base;
    if (const char* x = std::getenv("XDG_STATE_HOME"); x && *x) base = x;
    else if (const char* h = std::getenv("HOME"); h && *h)
        base = std::string(h) + "/.local/state";
    else return std::string();
    std::string dir = base + "/re-moct";
    std::error_code ec;
    fs::create_directories(fs::path(dir), ec);
    if (ec) return std::string();
    return dir + "/";
#endif
}

// A v4-shaped UUID (8-4-4-4-12 hex, version/variant nibbles set). Entropy is from
// std::rand — sufficient for a persisted, single-user probe identity; the value only
// has to be stable and unique-enough, not cryptographic. Seeded once from tickMs.
std::string genUuid() {
    static bool seeded = false;
    if (!seeded) { std::srand(port::tickMs()); seeded = true; }
    static const char* H = "0123456789abcdef";
    auto hex = [](int n) { std::string s; for (int i = 0; i < n; ++i) s += H[std::rand() & 15]; return s; };
    std::string u;
    u += hex(8); u += '-';
    u += hex(4); u += '-';
    u += '4'; u += hex(3); u += '-';                 // version 4
    u += H[8 + (std::rand() & 3)]; u += hex(3); u += '-';  // variant 10xx
    u += hex(12);
    return u;
}

// Read a JSON field as a string whether it arrives as a JSON string or a number
// (iHeart returns profileId as a number, sessionId as a string). "" if absent/null.
std::string jstr(const json& j, const char* key) {
    if (!j.contains(key)) return {};
    const json& v = j[key];
    if (v.is_string())         return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number_unsigned())return std::to_string(v.get<unsigned long long>());
    if (v.is_number())         return std::to_string(v.get<long long>());
    return {};
}

} // namespace

namespace IHeartIdentity {

std::string Identity::profileTail() const {
    if (profileId.empty()) return {};
    return profileId.size() <= 6 ? profileId : profileId.substr(profileId.size() - 6);
}

std::string identityPath() {
    std::string d = identityDir();
    return d.empty() ? std::string() : d + "iheart_probe_identity.json";
}

namespace {

void persist(const std::string& path, const Identity& id, const std::function<void(const std::string&)>& log) {
    json root = json::object();
    root["deviceId"]  = id.deviceId;
    root["profileId"] = id.profileId;   // full value lives in the identity file (NOT a capture)
    root["sessionId"] = id.sessionId;
    std::ofstream out(path, std::ios::trunc);
    if (out) { out << root.dump(2) << "\n"; }
    else if (log) log("iheart-identity: persist WRITE FAILED " + path);
}

} // namespace

Identity mintOrLoad(core::IHttp& http, const std::function<void(const std::string&)>& log) {
    auto logmsg = [&](const std::string& s) { if (log) log(s); };

    Identity id;
    std::string path = identityPath();
    if (path.empty()) {                       // can't persist -> can't satisfy the stability
        logmsg("iheart-identity: no app-data dir -> mint skipped (fall back to anon)");
        return id;                            // ok=false
    }

    // Load the persisted identity if present.
    {
        std::ifstream in(path);
        if (in) {
            json root;
            try {
                in >> root;
                if (root.is_object()) {
                    id.deviceId  = jstr(root, "deviceId");
                    id.profileId = jstr(root, "profileId");
                    id.sessionId = jstr(root, "sessionId");
                }
            } catch (...) { /* corrupt file -> treat as absent, re-mint below */ }
        }
    }

    // Stable identity already on disk: REUSE it, no POST. This is what gives the probe
    // a consistent identity + accruing history across runs.
    if (!id.profileId.empty()) {
        id.ok = true;
        logmsg("iheart-identity: reusing persisted profileId ..." + id.profileTail());
        return id;
    }

    // First mint (or a prior mint that never got a profileId): reuse the persisted
    // device UUID if we have one, else generate + persist it so a FAILED mint still
    // re-POSTs with the same UUID next run.
    if (id.deviceId.empty()) {
        id.deviceId = genUuid();
        persist(path, id, log);               // deviceId now durable even if the POST fails
    }

    // POST loginOrCreateOauthUser. Field set replicated VERBATIM from iheart-cli,
    // including its apparent `acessToken` spelling — the proven form; correcting it is
    // an untested change (open question, verify empirically).
    const std::string& uuid = id.deviceId;
    std::string body =
        "acessToken=anon"
        "&accessTokenType=anon"
        "&deviceId="   + uuid +
        "&deviceName=RE-MOCT-probe"
        "&host=webapp.US"
        "&oauthUuid="  + uuid +
        "&userName=anon" + uuid;

    // UA + timeout are SESSION-level across the plugin ABI (RemoctHttpReq carries
    // neither; the IHttp contract has requests-through-a-session ignore per-request
    // user_agent/timeout_ms — the session config wins). So set the browser UA and the
    // bound HERE, exactly like IHeartRadio::ensureSession — otherwise the mint would go
    // out with the impl-default UA and an UNBOUNDED timeout on the connect thread.
    core::HttpSessionConfig scfg;
    scfg.user_agent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                      "(KHTML, like Gecko) Chrome/124.0 Safari/537.36";
    scfg.timeout_ms = 5000;   // bound the mint so a hung account API never stalls the connect
    std::unique_ptr<core::IHttpSession> sess = http.openSession(scfg);
    if (!sess) { logmsg("iheart-identity: openSession failed -> anon fallback"); return id; }

    core::HttpRequest req;
    req.url          = "https://us.api.iheart.com/api/v1/account/loginOrCreateOauthUser";
    req.method       = "POST";
    req.body         = body;
    req.content_type = "application/x-www-form-urlencoded";
    req.headers      = {
        {"X-hostName", "webapp.US"},
        {"X-Locale",   "en-US"},
        {"Referer",    "https://www.iheart.com"},
        {"Origin",     "https://www.iheart.com"},
        {"Accept",     "application/json"},
    };
    req.max_body     = 1u * 1024u * 1024u;

    core::HttpResponse res = sess->fetch(req);
    if (!res.ok || res.status != 200) {
        logmsg("iheart-identity: mint HTTP " + std::to_string(res.status) + " -> anon fallback");
        return id;                            // ok=false; deviceId persisted for a same-UUID retry
    }

    json j;
    try { j = json::parse(res.body); }
    catch (...) { logmsg("iheart-identity: mint parse failed -> anon fallback"); return id; }

    id.profileId = jstr(j, "profileId");
    id.sessionId = jstr(j, "sessionId");
    if (id.profileId.empty()) {
        logmsg("iheart-identity: mint response had no profileId -> anon fallback");
        return id;                            // ok=false
    }

    id.ok = true;
    persist(path, id, log);
    logmsg("iheart-identity: minted anonymous profileId ..." + id.profileTail());
    return id;
}

} // namespace IHeartIdentity
