#pragma once

#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// Shared operational log for RE-MOCT.
//
// Replaces the three independent %TEMP%\remoct_stream.log append helpers that
// used to live in LastFm.cpp (lflog), StreamSource.cpp (slog) and
// UIManager.cpp (sclog). All diagnostic output now funnels through one sink so
// there is a single, timestamped, day-bounded operational log instead of three
// copies of an append-forever helper.
//
// Rotation is daily: each calendar day writes to its own dated file,
//   %TEMP%\remoct-YYYY-MM-DD.log
// On the first write of a new day the logger trims its dated files down to the
// most recent `keep` days (default 5), deleting the oldest. ISO dates sort
// lexically, so retention is a simple sort-and-delete. Non-dated files in %TEMP%
// (e.g. a legacy remoct_stream.log) are never touched.
//
// Every call is thread-safe; the streaming, scrobble and UI subsystems all write
// concurrently and the day-roll / trim is serialized under one mutex.
// ─────────────────────────────────────────────────────────────────────────────
namespace Log {

// Master on/off gate. Logging is ON by default (this is a real operational log,
// not debug-only scaffolding). Flip to false to silence all output for a quiet
// release build without removing any call sites.
void setEnabled(bool on);
bool enabled();

// Number of days of dated logs to retain (default 5). Safe to call once at
// startup; the default applies if never called.
void configure(int keep_days);

// Write one line. `component` is a short tag ("lastfm", "stream", "scrob", ...)
// rendered as "[component]". A local-time timestamp is prepended automatically;
// a trailing newline is added. Day-roll + retention are applied transparently.
void write(const char* component, const std::string& msg);

// printf-style convenience wrapper around write().
void writef(const char* component, const char* fmt, ...);

// Absolute path of today's active log file (%TEMP%\remoct-YYYY-MM-DD.log).
std::string path();

} // namespace Log

