/* remoct_plugin.h — the RE-MOCT plugin C ABI (Phase 4, slice (a): the frozen
 * contract). This is the SDK header shared by the host AND every plugin. It is
 * the ONE interface in the project that is a genuine BINARY contract, hard to
 * change after the fact — so the fundamentals are pinned here and evolution is
 * additive only (see "Versioning" below).
 *
 * Ratified design: docs/phase4-readiness.md §2 (Dos, 2026-07-04). Boundary A —
 * the first plugin is the streaming source (StreamSource entire; iHeart as its
 * headline capability). This header freezes the full v1 ABI now, against the
 * disposable sine test plugin, BEFORE StreamSource is extracted behind it.
 *
 * The five fundamentals (all four "expensive to change", gotten right up front):
 *   1. C linkage only. NO C++ across the line — no vtables, no name mangling,
 *      no std:: types, no templates. Only POD structs, primitives, UTF-8 C
 *      strings (NUL-terminated), and opaque void* handles. This header compiles
 *      as C (the sine plugin is pure C — the proof nothing C++ leaked in).
 *   2. noexcept everywhere. No C++ exception may cross the boundary. Errors are
 *      return codes / empty results (the IHeartRadio "never throws out of its
 *      public API" posture, now a hard ABI rule).
 *   3. Nobody frees across the boundary. Whoever ALLOCATES, frees. Host buffers
 *      for host->plugin fills (audio frames, metadata strings); host-owned
 *      transport results freed by a host function. No cross-heap free — the
 *      classic C-ABI landmine.
 *   4. Additive, struct-size-gated evolution (see Versioning).
 *   5. Model the real consumer (core::ISource + iHeart's real needs), not a
 *      hypothetical marketplace.
 *
 * Threading: read_frames runs on the AUDIO thread — it must be lock-free, must
 * not allocate, and must not block (exactly the StreamSource::readFrames
 * contract). Every other entry runs on the host/UI thread. open() is blocking
 * and slow; the host wraps it in its own async-connect thread (it already does
 * this for StreamSource today) — the plugin need not make open() async.
 */
#ifndef REMOCT_PLUGIN_H
#define REMOCT_PLUGIN_H

#include <stddef.h>   /* size_t   */
#include <stdint.h>   /* int32_t, uint32_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ── ABI version ──────────────────────────────────────────────────────────────
 * The MAJOR compatibility gate. The host compares a loaded plugin's
 * `abi_version` to this; a mismatch is refused at load (before create()).
 * Bump this only for a BREAKING change (a signature or semantics change);
 * additive growth does NOT bump it (see Versioning). */
#define REMOCT_ABI_VERSION 1u

/* Symbol visibility for the ONE exported entry point. Applied by the PLUGIN at
 * its definition of remoct_plugin_query; the host resolves the symbol
 * dynamically and never links it, so this is inert on the host side. */
#if defined(_WIN32)
  #define REMOCT_EXPORT __declspec(dllexport)
#else
  #define REMOCT_EXPORT __attribute__((visibility("default")))
#endif

/* ── POD mirror of core::SourceCaps ──────────────────────────────────────────
 * Declared, never faked — the host branches on these (int used as bool: 0/1). */
typedef struct RemoctSourceCaps {
    int32_t seekable;   /* seek_to() is honored           */
    int32_t finite;     /* has an end; duration_sec valid  */
    int32_t live;       /* real-time: a starved ring = buffering, never EOF */
} RemoctSourceCaps;

/* ── Host-provided services (injected at create) ─────────────────────────────
 * The host hands the plugin this C function table so the plugin reaches the
 * host's IHttp transport (WinINet/libcurl) instead of carrying its own — the
 * DI endgame architecture.md always described. The transitional core::http()
 * global lives in the HOST binary and is unreachable from a loaded .so/.dll,
 * which is exactly why service injection is mandatory here.
 *
 * Redirect policy mirror (== core::RedirectPolicy). */
enum { REMOCT_REDIRECT_FOLLOW_ALL = 0,
       REMOCT_REDIRECT_FOLLOW_NONE = 1,
       REMOCT_REDIRECT_FOLLOW_SAME_SCHEME = 2 };

typedef struct RemoctHttpSession RemoctHttpSession;   /* opaque HOST handle */

/* One HTTP request. All strings UTF-8. Bodies go over the wire as raw UTF-8
 * bytes — NEVER widened (widening corrupts non-ASCII scrobbles/queries). */
typedef struct RemoctHttpReq {
    const char*        url;
    const char*        method;         /* "GET"/"POST"; NULL => GET            */
    const char*        body;           /* POST payload (may contain NULs)      */
    size_t             body_len;
    const char*        content_type;
    const char* const* header_keys;    /* parallel arrays, header_count long   */
    const char* const* header_vals;
    size_t             header_count;
    int32_t            redirect;       /* REMOCT_REDIRECT_*                    */
    int32_t            pragma_no_cache;
    size_t             max_body;       /* 0 => unlimited; else cap in bytes    */
    int32_t            reject_truncated;
    /* Cancel token: a PLUGIN-owned flag. The plugin sets it nonzero from its
     * close()/stop path; the host transport polls it (acquire load) before the
     * open and before each chunk — the exact slice-4 granularity, so worst-case
     * abort latency stays one receive timeout, never the remaining body. Pinned
     * to int32_t so no std::atomic type crosses; the host reads it with acquire
     * semantics, the plugin writes it with release. NULL => not cancellable. */
    const int32_t*     cancel;
} RemoctHttpReq;

/* One HTTP response. HOST-allocated and HOST-freed (via http_response_free):
 * the plugin copies out what it needs, then frees. `body`/`final_url` are
 * BORROWED — valid only until http_response_free. */
typedef struct RemoctHttpResp {
    int32_t     ok;          /* transport succeeded                            */
    int32_t     cancelled;   /* aborted via req.cancel (body cleared)          */
    int32_t     read_error;  /* mid-body transport error (NOT EOF, NOT cap)    */
    int32_t     truncated;   /* hit max_body before EOF                        */
    long        status;      /* HTTP code (0 if unread)                        */
    const char* body;        /* byte buffer (borrowed)                         */
    size_t      body_len;
    const char* final_url;   /* post-redirect URL (borrowed)                   */
} RemoctHttpResp;

typedef struct RemoctHostServices {
    uint32_t struct_size;    /* sizeof(RemoctHostServices) at host build time  */
    void*    host_ctx;       /* opaque; pass back to the host fns below        */

    /* HTTP — the Phase-1 IHttp/IHttpSession seam, C-shimmed. A held session =
     * keep-alive reuse across fetches (WinINet InternetOpen handle / libcurl
     * CURLSH share handle). Returns NULL on failure (the plugin retries, as the
     * baselines' ensureSession did). */
    RemoctHttpSession* (*http_open_session)(void* host_ctx,
                                            const char* user_agent,
                                            int32_t timeout_ms);
    void  (*http_session_close)(RemoctHttpSession* s);
    /* Synchronous fetch. Fills *out (host-allocated fields); the plugin MUST
     * call http_response_free(out) when done. */
    void  (*http_fetch)(RemoctHttpSession* s, const RemoctHttpReq* req,
                        RemoctHttpResp* out);
    void  (*http_response_free)(RemoctHttpResp* resp);

    /* Optional host log sink (host_ctx-scoped slog). May be NULL. The iHeart
     * deep log stays PLUGIN-owned (it resolves its own dir) — this is only the
     * host's operational log. */
    void  (*log)(void* host_ctx, const char* msg);
} RemoctHostServices;

/* ── The plugin descriptor ───────────────────────────────────────────────────
 * Returned by the one exported entry. Field ORDER is part of the ABI: never
 * reorder or change a signature (that is a MAJOR bump); only APPEND new
 * function pointers at the end (struct-size-gated — see Versioning).
 *
 * REQUIRED (a plugin is undriveable without these; the loader rejects a
 * descriptor missing any): create, destroy, open, read_frames, close.
 * The rest are the core::ISource spine + the Phase-2-deferred metadata/config;
 * a host calls an optional pointer only after null-checking it AND confirming
 * struct_size reaches it (the "declare divergence, never fake" stance at the
 * ABI). The v1 sine plugin and the real stream plugin provide all of them. */
typedef struct RemoctPlugin {
    uint32_t    abi_version;    /* == REMOCT_ABI_VERSION the plugin built against */
    uint32_t    struct_size;    /* sizeof(RemoctPlugin) at plugin build time      */
    const char* name;           /* stable id, e.g. "stream"                       */
    const char* display_name;   /* "iHeart / Internet Radio"                      */

    /* ── lifecycle (host/UI thread) — REQUIRED ── */
    void*   (*create)(const RemoctHostServices* host, void* host_ctx); /* -> self */
    void    (*destroy)(void* self);
    int32_t (*handles_url)(void* self, const char* url);   /* 1 if it plays url    */
    int32_t (*open)(void* self, const char* url);          /* BLOCKING; 0 = ok     */

    /* ── audio (AUDIO thread — lock-free, no alloc, no block) — REQUIRED ──
     * Fill dst with frame_count stereo f32 frames @ 44100 Hz; return frames
     * written. A live source ALWAYS returns frame_count (silence-pads while
     * buffering/paused/underrun — its end is observable via state, not a short
     * read), exactly the StreamSource::readFrames contract. */
    uint32_t (*read_frames)(void* self, float* dst, uint32_t frame_count);

    /* ── transport (host/UI thread) — the core::ISource spine ── */
    void    (*get_caps)(void* self, RemoctSourceCaps* out);
    double  (*position_sec)(void* self);
    double  (*duration_sec)(void* self);   /* 0.0 when unknown (live)             */
    int32_t (*seek_to)(void* self, double seconds); /* 0 = refused/not seekable   */
    void    (*set_paused)(void* self, int32_t paused);
    int32_t (*is_buffering)(void* self);   /* 1 while (re)filling                 */
    void    (*close)(void* self);          /* REQUIRED; idempotent; joins machinery */

    /* ── metadata (host/UI thread; Phase-2 deferral #2) — OPTIONAL ──
     * Fill buf (cap bytes incl. NUL). Return the FULL length (snprintf
     * semantics) so the host can grow+retry. Empty ("", return 0) = none yet.
     * The string is COPIED into the host's buffer — it never crosses ownership
     * (the "nobody frees across the boundary" rule, applied to strings). */
    size_t  (*now_playing)(void* self, char* buf, size_t cap);  /* "Artist - Title" */
    size_t  (*art_url)(void* self, char* buf, size_t cap);
    size_t  (*last_error)(void* self, char* buf, size_t cap);

    /* ── config (host/UI thread) — OPTIONAL ──
     * One typed-enough knob channel, modeled on the one real knob. e.g.
     * ("prefer_digital","1"), ("deeplog","1"). NOT a general settings bus. */
    void    (*set_config)(void* self, const char* key, const char* value);
} RemoctPlugin;

/* ── The ONE exported symbol ─────────────────────────────────────────────────
 * A plugin exports exactly this (C linkage, undecorated name), returning a
 * pointer to a STATIC descriptor (valid for the module's lifetime — the host
 * never frees it). The host resolves it with dlsym/GetProcAddress. */
typedef const RemoctPlugin* (*RemoctQueryFn)(void);
REMOCT_EXPORT const RemoctPlugin* remoct_plugin_query(void);

/* ── Versioning / evolution contract ─────────────────────────────────────────
 * MAJOR gate: host refuses a plugin whose abi_version != REMOCT_ABI_VERSION.
 * ADDITIVE growth without a bump: a new capability is a NEW function pointer
 * APPENDED after set_config, never a reorder or a signature change. Both the
 * host and every plugin carry struct_size; the host reads only
 * min(host_size, plugin_size) worth of fields and null-checks any optional
 * pointer before calling it. So a v1 host loads a v1.1 plugin (ignores unknown
 * trailing fields) and a v1.1 host loads a v1 plugin (sees the smaller size,
 * treats new pointers as absent). This is the project's additive-only interface
 * discipline carried down to the binary layer. */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* REMOCT_PLUGIN_H */
