#pragma once

#include <string>
#include <functional>

#include "core/IHttp.h"   // mint POST goes through the platform seam (Linux-portable, no WinINet)

// ─────────────────────────────────────────────────────────────────────────────
// IHeartIdentity — probe-only minted *anonymous* iHeart account identity.
//
// The shipped digital handshake (StreamSource::hlsBuildDigitalUrl anon branch)
// carries NO profileId/skey and a fresh random listenerId per connect — the arm
// that eats the long "house-bin" ad backfill. The web player, by contrast, carries
// a real stable profileId (== skey). The one variant never tested live is a real
// *anonymous* profileId: shashfrankenstien/iheart-cli mints exactly this via the
// account endpoint below, keyed on a persisted device UUID.
//
// This helper wires that mint in as a flag-gated probe. It is invoked ONLY from the
// minted A/B arm while the deep log is enabled (see StreamSource::hlsConnect) — never
// in normal use. Any failure returns ok=false and the caller falls back to today's
// anonymous handshake, so a mint failure can never block or degrade playback.
//
// LOAD-BEARING: the identity is minted ONCE and PERSISTED, then reused across runs
// (mirroring iheart-cli's UUID file). The web player's edge may be identity stability
// + listening history, not the param on its own — so mint-fresh-every-connect would
// be the baseline with extra HTTP (a false negative). Persistence is a requirement.
//
// Endpoint:  POST https://us.api.iheart.com/api/v1/account/loginOrCreateOauthUser
// Persist:   %APPDATA%\RE-MOCT\iheart_probe_identity.json (Linux: XDG state twin)
//            The persisted file may hold FULL values; any *capture* file gets the
//            profileId TAIL only (profileTail()), never the full skey/sessionId.
// ─────────────────────────────────────────────────────────────────────────────
namespace IHeartIdentity {

struct Identity {
    bool        ok = false;     // a usable minted profileId is available for this connect
    std::string deviceId;       // persisted device UUID (reused across runs; both deviceId & oauthUuid)
    std::string profileId;      // minted anonymous profileId — used as BOTH profileId and aw_0_1st.skey
    std::string sessionId;      // minted sessionId (persisted; never written to a capture file)

    // Last 6 chars of profileId ("" if none) — the ONLY identity string safe to write
    // to the deep-log capture (idProfileTail). Never expose the full profileId/skey there.
    std::string profileTail() const;
};

// Lazily mint-or-load the persisted anonymous identity.
//   - First call with no persisted profileId: generate a device UUID, POST
//     loginOrCreateOauthUser, and persist {deviceId, profileId, sessionId}.
//   - Later calls: load and REUSE the persisted identity (stable across runs).
//   - Re-POSTs with the SAME persisted UUID only if the stored profileId is missing.
// Fail-closed: any network / non-200 / parse failure returns ok=false. Never throws.
// `log` (optional) receives human-readable trace lines; profileId is logged TAIL-only.
Identity mintOrLoad(core::IHttp& http, const std::function<void(const std::string&)>& log = {});

// Absolute path of the persisted identity file (for diagnostics/tests). May be "".
std::string identityPath();

} // namespace IHeartIdentity
