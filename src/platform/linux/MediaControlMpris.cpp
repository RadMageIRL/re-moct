// MediaControlMpris.cpp - Linux MPRIS impl of core::IMediaControl (osmedia-seam).
// org.mpris.MediaPlayer2 + .Player over sd-bus (-lsystemd), proven in
// probes/osmedia/FINDINGS.md. playerctl and desktop widgets drive it.
//
// Threading: the bus is serviced ONLY on the UI thread via pump()
// (sd_bus_process + a non-blocking poll each main-loop tick) - the getch()
// timeout loop is not a poll() the fd can fold into, so we pump per tick
// (~80 ms latency, imperceptible). Method callbacks therefore run on the UI
// thread and merely fire() the registered handler (enqueue); the same drain
// applies them. Outbound updates are UI-thread too, so the bus has ONE user and
// no concurrency. If the session bus is unavailable the impl is inert (no crash).
#include "core/IMediaControl.h"
#include "NoopMediaControl.h"

#include <systemd/sd-bus.h>
#include <poll.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

const char* kPath   = "/org/mpris/MediaPlayer2";
const char* kPlayer = "org.mpris.MediaPlayer2.Player";

class MprisMediaControl : public core::IMediaControl {
public:
    MprisMediaControl() { init(); }
    ~MprisMediaControl() override {
        if (bus_) { sd_bus_flush(bus_); sd_bus_unref(bus_); }
    }
    bool ok() const { return bus_ != nullptr; }

    void setCommandHandler(std::function<void(core::MediaEvent)> h) override {
        handler_ = std::move(h);
    }

    // Write the bundled logo to a write-once cache file and remember its file://
    // URL for the kMediaArtLogo floor. Once per session (called at wire time),
    // never per track. Best-effort: on failure logo_url_ stays empty (blank art).
    void setDefaultArt(std::vector<uint8_t> jpeg) override {
        logo_url_ = writeCache("remoct_logo.jpg", jpeg);
    }

    // The remoct cache dir (created), or "" if neither XDG_CACHE_HOME nor HOME.
    static std::string cacheDir() {
        std::string dir;
        if (const char* x = getenv("XDG_CACHE_HOME"); x && *x) dir = std::string(x) + "/remoct";
        else if (const char* h = getenv("HOME"); h && *h) dir = std::string(h) + "/.cache/remoct";
        else return {};
        if (auto slash = dir.rfind('/'); slash != std::string::npos)
            mkdir(dir.substr(0, slash).c_str(), 0700);   // parent, ignore EEXIST
        mkdir(dir.c_str(), 0700);
        return dir;
    }
    // Write bytes to <cache>/fname and return its file:// URL ("" on failure).
    // The logo is write-once (static name, called once); the per-track cover
    // reuses the same name (remoct_cover.jpg), overwritten on each track change
    // - not per tick, since updateNowPlaying only fires on a change.
    static std::string writeCache(const std::string& fname, const std::vector<uint8_t>& bytes) {
        if (bytes.empty()) return {};
        std::string dir = cacheDir();
        if (dir.empty()) return {};
        std::string path = dir + "/" + fname;
        if (FILE* f = fopen(path.c_str(), "wb")) {
            bool ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
            ok = (fclose(f) == 0) && ok;
            if (ok) return "file://" + path;
        }
        return {};
    }

    void updateNowPlaying(const core::MediaMeta& m, core::MediaStatus st,
                          double pos, double dur) override {
        if (!bus_) return;
        artist_ = m.artist; title_ = m.title; album_ = m.album; art_ = m.art;
        // Per-track embedded cover -> a cache file whose file:// URL wins over the
        // radio URL / logo in p_metadata (osmedia-art-floor: album art first).
        cover_url_ = writeCache("remoct_cover.jpg", m.art_bytes);
        length_us_ = (dur > 0) ? (int64_t)(dur * 1e6) : 0;
        position_us_ = (int64_t)(pos * 1e6);
        status_ = st;
        ++trackid_;                          // new track -> new mpris:trackid + Seeked reset
        // Metadata + PlaybackStatus changed (NOT Position: poll-only by spec).
        sd_bus_emit_properties_changed(bus_, kPath, kPlayer,
                                       "Metadata", "PlaybackStatus", nullptr);
        emitSeeked();                        // a track change is a discontinuity
    }

    void updatePosition(double pos, double dur, core::MediaStatus st) override {
        if (!bus_) return;
        position_us_ = (int64_t)(pos * 1e6);
        if (dur > 0) length_us_ = (int64_t)(dur * 1e6);
        // Refresh the cached value the Position getter returns; emit a change
        // signal ONLY when the status flips (never per-tick - Position is
        // poll-only and per-tick PropertiesChanged would be ~12 Hz bus spam).
        if (st != status_) {
            status_ = st;
            sd_bus_emit_properties_changed(bus_, kPath, kPlayer, "PlaybackStatus", nullptr);
        }
    }

    void clear() override {
        if (!bus_) return;
        status_ = core::MediaStatus::Stopped;
        artist_.clear(); title_.clear(); album_.clear(); art_.clear(); cover_url_.clear();
        length_us_ = 0; position_us_ = 0;
        sd_bus_emit_properties_changed(bus_, kPath, kPlayer,
                                       "Metadata", "PlaybackStatus", nullptr);
    }

    void pump() override {
        if (!bus_) return;
        for (;;) {
            int r = sd_bus_process(bus_, nullptr);   // dispatch queued messages
            if (r <= 0) break;                       // r>0 = made progress, keep draining
        }
        // Non-blocking poll: give the fd a 0 ms glance so a just-queued reply
        // flushes; the loop's getch() timeout provides the actual cadence.
        struct pollfd pfd{ sd_bus_get_fd(bus_), (short)sd_bus_get_events(bus_), 0 };
        if (pfd.fd >= 0) poll(&pfd, 1, 0);
    }

    // ── sd-bus statics (userdata = this; all run on the UI thread via pump) ──
    static MprisMediaControl* self(void* ud) { return static_cast<MprisMediaControl*>(ud); }

    static int m_play(sd_bus_message* m, void* ud, sd_bus_error*) {
        self(ud)->fire(core::MediaCommand::Play);        return sd_bus_reply_method_return(m, ""); }
    static int m_pause(sd_bus_message* m, void* ud, sd_bus_error*) {
        self(ud)->fire(core::MediaCommand::Pause);       return sd_bus_reply_method_return(m, ""); }
    static int m_playpause(sd_bus_message* m, void* ud, sd_bus_error*) {
        self(ud)->fire(core::MediaCommand::TogglePause); return sd_bus_reply_method_return(m, ""); }
    static int m_stop(sd_bus_message* m, void* ud, sd_bus_error*) {
        self(ud)->fire(core::MediaCommand::Stop);        return sd_bus_reply_method_return(m, ""); }
    static int m_next(sd_bus_message* m, void* ud, sd_bus_error*) {
        self(ud)->fire(core::MediaCommand::Next);        return sd_bus_reply_method_return(m, ""); }
    static int m_prev(sd_bus_message* m, void* ud, sd_bus_error*) {
        self(ud)->fire(core::MediaCommand::Previous);    return sd_bus_reply_method_return(m, ""); }
    static int m_noop(sd_bus_message* m, void*, sd_bus_error*) {
        return sd_bus_reply_method_return(m, ""); }

    static int m_seek(sd_bus_message* m, void* ud, sd_bus_error*) {   // relative, microseconds
        int64_t off = 0;
        if (sd_bus_message_read(m, "x", &off) >= 0)
            self(ud)->fire(core::MediaCommand::SeekByRel, (double)off / 1e6);
        return sd_bus_reply_method_return(m, "");
    }
    static int m_setpos(sd_bus_message* m, void* ud, sd_bus_error*) { // absolute, (o, x us)
        const char* track = nullptr; int64_t pos = 0;
        if (sd_bus_message_read(m, "ox", &track, &pos) >= 0)
            self(ud)->fire(core::MediaCommand::SeekToAbs, (double)pos / 1e6);
        return sd_bus_reply_method_return(m, "");
    }

    static int p_status(sd_bus*, const char*, const char*, const char*,
                        sd_bus_message* reply, void* ud, sd_bus_error*) {
        const char* s = "Stopped";
        switch (self(ud)->status_) {
            case core::MediaStatus::Playing: s = "Playing"; break;
            case core::MediaStatus::Paused:  s = "Paused";  break;
            default: break;
        }
        return sd_bus_message_append(reply, "s", s);
    }
    static int p_position(sd_bus*, const char*, const char*, const char*,
                          sd_bus_message* reply, void* ud, sd_bus_error*) {
        return sd_bus_message_append(reply, "x", self(ud)->position_us_);
    }
    static int p_true(sd_bus*, const char*, const char*, const char*,
                      sd_bus_message* reply, void*, sd_bus_error*) {
        return sd_bus_message_append(reply, "b", 1);
    }
    static int p_false(sd_bus*, const char*, const char*, const char*,
                       sd_bus_message* reply, void*, sd_bus_error*) {
        return sd_bus_message_append(reply, "b", 0);
    }
    static int p_identity(sd_bus*, const char*, const char*, const char*,
                          sd_bus_message* reply, void*, sd_bus_error*) {
        return sd_bus_message_append(reply, "s", "RE-MOCT");
    }
    static int p_rate(sd_bus*, const char*, const char*, const char*,
                      sd_bus_message* reply, void*, sd_bus_error*) {
        return sd_bus_message_append(reply, "d", 1.0);
    }
    static int p_volume(sd_bus*, const char*, const char*, const char*,
                        sd_bus_message* reply, void*, sd_bus_error*) {
        return sd_bus_message_append(reply, "d", 1.0);
    }
    static int p_metadata(sd_bus*, const char*, const char*, const char*,
                          sd_bus_message* reply, void* ud, sd_bus_error*) {
        MprisMediaControl* t = self(ud);
        int r;
        if ((r = sd_bus_message_open_container(reply, 'a', "{sv}")) < 0) return r;
        char trackpath[64];
        std::snprintf(trackpath, sizeof(trackpath), "/org/remoct/track/%lld",
                      (long long)t->trackid_);
        entryOV(reply, "mpris:trackid", trackpath);
        if (t->length_us_ > 0) entryX(reply, "mpris:length", t->length_us_);
        if (!t->title_.empty())  entrySV(reply, "xesam:title",  t->title_.c_str());
        if (!t->album_.empty())  entrySV(reply, "xesam:album",  t->album_.c_str());
        if (!t->artist_.empty()) entryAS(reply, "xesam:artist", t->artist_.c_str());
        // Precedence: per-track embedded cover (cache file) -> radio URL -> logo
        // floor (sentinel). Empty -> no artUrl (blank).
        const std::string& art_url =
            !t->cover_url_.empty()             ? t->cover_url_
          : (t->art_ == core::kMediaArtLogo)   ? t->logo_url_
                                               : t->art_;
        if (!art_url.empty())    entrySV(reply, "mpris:artUrl", art_url.c_str());
        return sd_bus_message_close_container(reply);
    }

private:
    void fire(core::MediaCommand c, double secs = 0.0) {
        if (handler_) handler_({c, secs});
    }
    void emitSeeked() {
        if (bus_) sd_bus_emit_signal(bus_, kPath, kPlayer, "Seeked", "x", position_us_);
    }

    static void entrySV(sd_bus_message* r, const char* k, const char* v) {
        sd_bus_message_open_container(r, 'e', "sv");
        sd_bus_message_append(r, "s", k);
        sd_bus_message_append(r, "v", "s", v);
        sd_bus_message_close_container(r);
    }
    static void entryOV(sd_bus_message* r, const char* k, const char* v) {
        sd_bus_message_open_container(r, 'e', "sv");
        sd_bus_message_append(r, "s", k);
        sd_bus_message_append(r, "v", "o", v);
        sd_bus_message_close_container(r);
    }
    static void entryX(sd_bus_message* r, const char* k, int64_t v) {
        sd_bus_message_open_container(r, 'e', "sv");
        sd_bus_message_append(r, "s", k);
        sd_bus_message_append(r, "v", "x", v);
        sd_bus_message_close_container(r);
    }
    static void entryAS(sd_bus_message* r, const char* k, const char* v) {
        sd_bus_message_open_container(r, 'e', "sv");
        sd_bus_message_append(r, "s", k);
        sd_bus_message_open_container(r, 'v', "as");
        sd_bus_message_append(r, "as", 1, v);
        sd_bus_message_close_container(r);
        sd_bus_message_close_container(r);
    }

    void init() {
        if (sd_bus_open_user(&bus_) < 0) { bus_ = nullptr; return; }
        static const sd_bus_vtable root_vtable[] = {
            SD_BUS_VTABLE_START(0),
            SD_BUS_METHOD("Raise", "", "", m_noop, 0),
            SD_BUS_METHOD("Quit",  "", "", m_noop, 0),
            SD_BUS_PROPERTY("Identity",     "s", p_identity, 0, 0),
            SD_BUS_PROPERTY("CanQuit",      "b", p_false,    0, 0),
            SD_BUS_PROPERTY("CanRaise",     "b", p_false,    0, 0),
            SD_BUS_PROPERTY("HasTrackList", "b", p_false,    0, 0),
            SD_BUS_VTABLE_END
        };
        static const sd_bus_vtable player_vtable[] = {
            SD_BUS_VTABLE_START(0),
            SD_BUS_METHOD("Play",      "",  "", m_play,      0),
            SD_BUS_METHOD("Pause",     "",  "", m_pause,     0),
            SD_BUS_METHOD("PlayPause", "",  "", m_playpause, 0),
            SD_BUS_METHOD("Stop",      "",  "", m_stop,      0),
            SD_BUS_METHOD("Next",      "",  "", m_next,      0),
            SD_BUS_METHOD("Previous",  "",  "", m_prev,      0),
            SD_BUS_METHOD("Seek",        "x",  "", m_seek,   0),
            SD_BUS_METHOD("SetPosition", "ox", "", m_setpos, 0),
            SD_BUS_PROPERTY("PlaybackStatus", "s",     p_status,   0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
            SD_BUS_PROPERTY("Metadata",       "a{sv}", p_metadata, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
            SD_BUS_PROPERTY("Position",       "x",     p_position, 0, 0),   // poll-only, no signal
            SD_BUS_PROPERTY("Rate",           "d",     p_rate,     0, 0),
            SD_BUS_PROPERTY("MinimumRate",    "d",     p_rate,     0, 0),
            SD_BUS_PROPERTY("MaximumRate",    "d",     p_rate,     0, 0),
            SD_BUS_PROPERTY("Volume",         "d",     p_volume,   0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
            SD_BUS_PROPERTY("CanGoNext",     "b", p_true, 0, 0),
            SD_BUS_PROPERTY("CanGoPrevious", "b", p_true, 0, 0),
            SD_BUS_PROPERTY("CanPlay",       "b", p_true, 0, 0),
            SD_BUS_PROPERTY("CanPause",      "b", p_true, 0, 0),
            SD_BUS_PROPERTY("CanSeek",       "b", p_true, 0, 0),
            SD_BUS_PROPERTY("CanControl",    "b", p_true, 0, 0),
            SD_BUS_VTABLE_END
        };
        if (sd_bus_add_object_vtable(bus_, &s_root_, kPath,
                "org.mpris.MediaPlayer2", root_vtable, this) < 0 ||
            sd_bus_add_object_vtable(bus_, &s_player_, kPath,
                kPlayer, player_vtable, this) < 0 ||
            sd_bus_request_name(bus_, "org.mpris.MediaPlayer2.remoct", 0) < 0) {
            sd_bus_unref(bus_); bus_ = nullptr; return;
        }
    }

    sd_bus*      bus_ = nullptr;
    sd_bus_slot* s_root_ = nullptr;
    sd_bus_slot* s_player_ = nullptr;
    std::function<void(core::MediaEvent)> handler_;
    core::MediaStatus status_ = core::MediaStatus::Stopped;
    std::string artist_, title_, album_, art_;
    std::string logo_url_;    // cached file:// URL for the kMediaArtLogo floor
    std::string cover_url_;   // cached file:// URL for the current track's embedded cover
    int64_t length_us_ = 0, position_us_ = 0;
    long long trackid_ = 0;
};

} // namespace

namespace core {
IMediaControl& mediaControl() {
    static MprisMediaControl inst;
    static NoopMediaControl  noop;
    return inst.ok() ? static_cast<IMediaControl&>(inst)
                     : static_cast<IMediaControl&>(noop);   // no session bus -> inert
}
} // namespace core
