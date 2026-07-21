#include "PodcastClient.h"
#include "core/IHttp.h"

// GET the feed over the seam, then parse. Mirrors RadioBrowser::httpGet's shape.
// Bounds are feed-sized, not radio-sized: real feeds run large (~5 MB JRE,
// ~22 MB Adam Carolla), so the cap is 64 MB and the timeout is loose because the
// caller runs this on a worker thread (no UI freeze). A friendly UA is set
// because some feed hosts 403 an empty/odd agent.
PodcastClient::Result PodcastClient::fetch(const std::string& url) {
    Result r;
    if (url.empty()) return r;

    core::HttpRequest req;
    req.url        = url;
    req.timeout_ms = 30000;                 // generous; off the UI thread
    req.max_body   = 64u * 1024 * 1024;     // 64 MB: covers the largest real feeds with headroom
    req.user_agent = "RE-MOCT/1.4 (podcast client)";

    core::HttpResponse resp = core::http().fetch(req);
    r.fetched = resp.ok && !resp.body.empty();
    if (r.fetched) r.feed = parsePodcastFeed(resp.body);
    return r;
}

bool PodcastClient::download(const std::string& url, const std::string& dest_path,
                             const core::ProgressFn& progress, const std::int32_t* cancel) {
    if (url.empty() || dest_path.empty()) return false;
    core::HttpRequest req;
    req.url        = url;
    // A stall guard (per-read / connect), NOT a whole-transfer deadline: a big
    // episode on a slow-but-alive link keeps downloading; only a stall aborts.
    req.timeout_ms = 30000;
    req.user_agent = "RE-MOCT/1.4 (podcast client)";
    req.cancel     = cancel;
    // No max_body: the body streams straight to disk, so size is not a memory concern.
    return core::http().fetchToFile(req, dest_path, progress).ok;
}
