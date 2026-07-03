#pragma once

#include <string>
#include <vector>
#include <utility>

// Minimal Last.fm Scrobbling 2.0 client. All calls are MD5-signed (sorted params
// + shared secret). Auth uses the desktop/web token flow: request a token, the
// user authorizes it in a browser, then we exchange it for a session key.
class LastFm {
public:
    // ── Auth ──
    // Step 1: get a request token + the URL the user must authorize in a browser.
    static bool requestToken(const std::string& api_key, const std::string& secret,
                             std::string& token_out, std::string& authorize_url_out);
    // Step 2: after the user authorizes, exchange the token for a session key.
    static bool getSession(const std::string& api_key, const std::string& secret,
                           const std::string& token,
                           std::string& session_key_out, std::string& username_out);

    // ── Scrobbling (wired into playback next step) ──
    static bool updateNowPlaying(const std::string& api_key, const std::string& secret,
                                 const std::string& session_key,
                                 const std::string& artist, const std::string& track,
                                 const std::string& album = "");
    static bool scrobble(const std::string& api_key, const std::string& secret,
                         const std::string& session_key,
                         const std::string& artist, const std::string& track,
                         long timestamp, bool chosen_by_user,
                         const std::string& album = "");

private:
    using Params = std::vector<std::pair<std::string, std::string>>;

    static std::string md5Hex(const std::string& s);             // OS CryptoAPI MD5
    static std::string sign(Params& params, const std::string& secret);  // sorts, returns api_sig
    static std::string urlEncode(const std::string& s);
    static std::string httpGet(const std::string& url);
    static std::string httpPost(const std::string& path, const std::string& body);
    static bool         postWriteCall(const Params& signed_params, const std::string& api_sig);
};

