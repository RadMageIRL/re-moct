#pragma once
#ifdef _WIN32

#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// Native ListenBrainz submission client.
//
// Unlike Last.fm there is no request signing and no token→session handshake: the
// user pastes their personal user token (from https://listenbrainz.org/settings/)
// and it travels as an "Authorization: Token <token>" header. Submissions are
// JSON POSTs to /1/submit-listens. Diagnostics route through Log::write("listenbrainz").
//
// Submit policy (mirrors what updateScrobbler already enforces for Last.fm):
//   - playingNow   → listen_type "playing_now": track metadata only, NO timestamp.
//   - submitSingle → listen_type "single": a completed listen with listened_at,
//     sent once the user has heard half the track or 4 minutes, whichever is less.
// ─────────────────────────────────────────────────────────────────────────────
class ListenBrainz {
public:
    // Validate a user token. On success returns true and fills user_out with the
    // associated MusicBrainz username. GET /1/validate-token.
    static bool validateToken(const std::string& token, std::string& user_out);

    // listen_type "playing_now": no listened_at is sent.
    static bool playingNow(const std::string& token,
                           const std::string& artist, const std::string& track,
                           const std::string& album = "");

    // listen_type "single": a completed listen. listened_at is unix epoch seconds.
    static bool submitSingle(const std::string& token,
                             const std::string& artist, const std::string& track,
                             long listened_at, const std::string& album = "");

private:
    // Build the submit-listens JSON body. listen_type is "single" or "playing_now".
    // listened_at is included only when non-null (single); omitted for playing_now.
    static std::string buildSubmitBody(const std::string& listen_type,
                                       const std::string& artist, const std::string& track,
                                       const std::string& album, const long* listened_at);

    static std::string httpPost(const std::string& path, const std::string& json_body,
                                const std::string& token, long* http_status);
    static std::string httpGet(const std::string& path,
                               const std::string& token, long* http_status);
};

#endif // _WIN32
