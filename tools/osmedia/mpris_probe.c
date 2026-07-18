// mpris_probe.c - osmedia-phase0-probe, Probe 2 (RETAINED ARTIFACT, not production).
//
// Question: with sd-bus (libsystemd), can we export a minimal
// org.mpris.MediaPlayer2 + .Player object on the session bus that playerctl
// sees and drives? And how does D-Bus dispatch integrate (own thread, or fold
// its fd into an existing poll)?
//
// This exports the two MPRIS interfaces with a few properties (PlaybackStatus,
// Metadata, CanGoNext/Previous/Play/Pause/Control) and the Play/Pause/PlayPause/
// Next/Previous/Stop methods as STUBS that just log. Real command marshalling to
// the UI thread is DESIGN for the next brief - not built here.
//
// Build: see build-mpris.sh. Drive: see drive-mpris.sh (dbus-run-session +
// playerctl). Runs until SIGTERM/SIGINT.

#include <systemd/sd-bus.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <poll.h>

static volatile sig_atomic_t g_run = 1;
static void on_sig(int s) { (void)s; g_run = 0; }

// ---- method stubs (log only; the real sink marshals to the UI thread) ----
static int m_log(sd_bus_message *m, void *ud, sd_bus_error *e) {
    (void)ud; (void)e;
    fprintf(stderr, "  >>> method: %s.%s\n",
            sd_bus_message_get_interface(m), sd_bus_message_get_member(m));
    fflush(stderr);
    return sd_bus_reply_method_return(m, "");
}

// ---- property getters ----
static int p_status(sd_bus *b, const char *p, const char *i, const char *prop,
                    sd_bus_message *reply, void *ud, sd_bus_error *e) {
    (void)b;(void)p;(void)i;(void)prop;(void)ud;(void)e;
    return sd_bus_message_append(reply, "s", "Playing");
}
static int p_identity(sd_bus *b, const char *p, const char *i, const char *prop,
                      sd_bus_message *reply, void *ud, sd_bus_error *e) {
    (void)b;(void)p;(void)i;(void)prop;(void)ud;(void)e;
    return sd_bus_message_append(reply, "s", "RE-MOCT");
}
static int p_true(sd_bus *b, const char *p, const char *i, const char *prop,
                  sd_bus_message *reply, void *ud, sd_bus_error *e) {
    (void)b;(void)p;(void)i;(void)prop;(void)ud;(void)e;
    return sd_bus_message_append(reply, "b", 1);
}
static int p_false(sd_bus *b, const char *p, const char *i, const char *prop,
                   sd_bus_message *reply, void *ud, sd_bus_error *e) {
    (void)b;(void)p;(void)i;(void)prop;(void)ud;(void)e;
    return sd_bus_message_append(reply, "b", 0);
}

// Metadata a{sv}: mpris:trackid (o), xesam:title (s), xesam:artist (as).
static int p_metadata(sd_bus *b, const char *p, const char *i, const char *prop,
                      sd_bus_message *reply, void *ud, sd_bus_error *e) {
    (void)b;(void)p;(void)i;(void)prop;(void)ud;(void)e;
    int r;
    if ((r = sd_bus_message_open_container(reply, 'a', "{sv}")) < 0) return r;

    if ((r = sd_bus_message_open_container(reply, 'e', "sv")) < 0) return r;
    sd_bus_message_append(reply, "s", "mpris:trackid");
    sd_bus_message_append(reply, "v", "o", "/org/remoct/track/0");
    sd_bus_message_close_container(reply);

    if ((r = sd_bus_message_open_container(reply, 'e', "sv")) < 0) return r;
    sd_bus_message_append(reply, "s", "xesam:title");
    sd_bus_message_append(reply, "v", "s", "Probe Title");
    sd_bus_message_close_container(reply);

    if ((r = sd_bus_message_open_container(reply, 'e', "sv")) < 0) return r;
    sd_bus_message_append(reply, "s", "xesam:artist");
    sd_bus_message_open_container(reply, 'v', "as");
    sd_bus_message_append(reply, "as", 1, "Probe Artist");
    sd_bus_message_close_container(reply);
    sd_bus_message_close_container(reply);

    return sd_bus_message_close_container(reply);
}

static const sd_bus_vtable root_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Raise", "", "", m_log, 0),
    SD_BUS_METHOD("Quit",  "", "", m_log, 0),
    SD_BUS_PROPERTY("Identity",     "s", p_identity, 0, 0),
    SD_BUS_PROPERTY("CanQuit",      "b", p_false,    0, 0),
    SD_BUS_PROPERTY("CanRaise",     "b", p_false,    0, 0),
    SD_BUS_PROPERTY("HasTrackList", "b", p_false,    0, 0),
    SD_BUS_VTABLE_END
};

static const sd_bus_vtable player_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Play",      "", "", m_log, 0),
    SD_BUS_METHOD("Pause",     "", "", m_log, 0),
    SD_BUS_METHOD("PlayPause", "", "", m_log, 0),
    SD_BUS_METHOD("Stop",      "", "", m_log, 0),
    SD_BUS_METHOD("Next",      "", "", m_log, 0),
    SD_BUS_METHOD("Previous",  "", "", m_log, 0),
    SD_BUS_PROPERTY("PlaybackStatus", "s",     p_status,   0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Metadata",       "a{sv}", p_metadata, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("CanGoNext",     "b", p_true, 0, 0),
    SD_BUS_PROPERTY("CanGoPrevious", "b", p_true, 0, 0),
    SD_BUS_PROPERTY("CanPlay",       "b", p_true, 0, 0),
    SD_BUS_PROPERTY("CanPause",      "b", p_true, 0, 0),
    SD_BUS_PROPERTY("CanControl",    "b", p_true, 0, 0),
    SD_BUS_VTABLE_END
};

int main(void) {
    signal(SIGTERM, on_sig);
    signal(SIGINT,  on_sig);

    sd_bus *bus = NULL;
    sd_bus_slot *s1 = NULL, *s2 = NULL;
    int r;

    if ((r = sd_bus_open_user(&bus)) < 0) {
        fprintf(stderr, "sd_bus_open_user: %s\n", strerror(-r)); return 1;
    }
    r = sd_bus_add_object_vtable(bus, &s1, "/org/mpris/MediaPlayer2",
                                 "org.mpris.MediaPlayer2", root_vtable, NULL);
    if (r < 0) { fprintf(stderr, "add root vtable: %s\n", strerror(-r)); return 1; }
    r = sd_bus_add_object_vtable(bus, &s2, "/org/mpris/MediaPlayer2",
                                 "org.mpris.MediaPlayer2.Player", player_vtable, NULL);
    if (r < 0) { fprintf(stderr, "add player vtable: %s\n", strerror(-r)); return 1; }

    r = sd_bus_request_name(bus, "org.mpris.MediaPlayer2.remoct", 0);
    if (r < 0) { fprintf(stderr, "request_name: %s\n", strerror(-r)); return 1; }

    // Dispatch-model report: sd-bus exposes a single fd + a timeout, so it folds
    // into any existing poll() loop - no dedicated thread required. We show the
    // fd here, then run the canonical poll-integrated loop.
    int fd = sd_bus_get_fd(bus);
    fprintf(stderr, "mpris_probe: exported org.mpris.MediaPlayer2.remoct, bus fd=%d\n", fd);
    fflush(stderr);

    while (g_run) {
        // Drain all ready work first (process returns >0 while it made progress).
        r = sd_bus_process(bus, NULL);
        if (r < 0) { fprintf(stderr, "process: %s\n", strerror(-r)); break; }
        if (r > 0) continue;                 // more may be queued
        // Idle: block on the bus fd with sd-bus's own timeout - the exact shape
        // that folds into a larger poll set (poll on fd | events, timeout).
        uint64_t usec; int ev = sd_bus_get_events(bus);
        struct pollfd pfd = { .fd = fd, .events = (short)ev, .revents = 0 };
        if (sd_bus_get_timeout(bus, &usec) < 0) usec = (uint64_t)-1;
        int tmo = (usec == (uint64_t)-1) ? -1 : (int)(usec / 1000);
        poll(&pfd, 1, tmo < 0 ? 200 : tmo);  // 200 ms cap so SIGTERM is prompt
    }

    fprintf(stderr, "mpris_probe: exiting\n");
    sd_bus_slot_unref(s2);
    sd_bus_slot_unref(s1);
    sd_bus_unref(bus);
    return 0;
}
