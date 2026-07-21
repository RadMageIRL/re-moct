#pragma once

// ─── Podcast RSS/Atom feed parser ────────────────────────────────────────────
//
// A standalone, PURE feed parser: raw feed XML (a std::string) in, a PodcastFeed
// struct out. No I/O, no globals, no throw — the whole point of Podcasts slice 1
// is to prove the one genuinely-new, genuinely-messy piece (XML-in-the-wild) in
// isolation before any UI/HTTP wiring lands (slice 2+).
//
// Design follows the tree's own precedent, not a heavy XML library: focused tag
// extraction, the same philosophy as PlaylistManager's XSPF read
// (PlaylistManager.cpp:473). Podcast feeds are regular enough that robust
// extraction handles them; a full DOM parser would be over-engineering and a new
// dependency. The XSPF helper only reads <tag>body</tag>; feeds also need
// attribute reads (<enclosure url=…>, <itunes:image href=…>), namespaced tags
// (itunes:*), CDATA, and HTML-in-descriptions, so this unit carries its own small
// helper set. xml_unescape is DUPLICATED here (extended for numeric entities)
// rather than shared out of PlaylistManager.cpp — sharing it would mean editing
// that file to expose a currently file-static helper, which this slice avoids.
//
// FORMAT SUPPORT: RSS 2.0 (<rss><channel><item>) fully. Atom (<feed><entry>) is
// DEFERRED to a later slice — RSS is ~95% of podcasts; an Atom feed simply parses
// to ok=false here (no <item> found), never a crash. Note the defer, don't build it.
//
// DEFENSIVE CONTRACT: never crash, never throw, never hang. Missing fields
// degrade (empty / 0), a malformed <item> is skipped and counted, an <item> with
// no enclosure is skipped (unplayable) and counted, garbage/empty input yields
// PodcastFeed{ ok=false }.

#include <string>
#include <vector>
#include <cstdint>
#include <cctype>
#include <utility>

struct PodcastEpisode {
    std::string  title;
    std::string  audio_url;          // <enclosure url="…"> — the playable/downloadable file
    std::string  guid;               // <guid> body if present (identity key candidate 1)
    std::string  pub_date_raw;       // <pubDate> as-found (kept verbatim regardless of parse)
    std::int64_t pub_date_unix = 0;  // epoch seconds, 0 if unparseable
    std::int64_t duration_sec  = 0;  // <itunes:duration> (sec / MM:SS / HH:MM:SS), 0 if absent/bad
    std::string  description;        // <description> or <itunes:summary>, CDATA/HTML stripped
    std::string  image_url;          // episode <itunes:image href>; "" -> inherit feed art
    // Identity key resolved at use (slice 3): guid ? guid : audio_url ? audio_url
    // : hash(title + pub_date_raw). Not computed here — the parser just fills the inputs.
};

struct PodcastFeed {
    std::string title;               // channel <title>
    std::string author;              // <itunes:author> / <managingEditor>
    std::string description;         // channel <description> / <itunes:summary>
    std::string image_url;           // feed <itunes:image href> or <image><url>
    std::string link;                // channel <link> (the show's website)
    std::vector<PodcastEpisode> episodes;  // in feed order (newest-first as given; not re-sorted in v1)
    bool ok = false;                 // channel found AND >= 1 valid episode parsed
    int  skipped_episodes = 0;       // malformed / enclosure-less <item> blocks skipped
};

// Stable identity key for an episode (slice 3): guid -> audio_url -> hash of
// (title + pub_date_raw). Used by both resume position and played-state. Pure; the
// hash fallback is 64-bit FNV-1a rendered as "h:<hex>" so it can't collide with a
// real guid/url. An episode with none of the three inputs returns "".
inline std::string podcastEpisodeId(const PodcastEpisode& ep) {
    if (!ep.guid.empty())      return ep.guid;
    if (!ep.audio_url.empty()) return ep.audio_url;
    if (ep.title.empty() && ep.pub_date_raw.empty()) return "";
    std::string s = ep.title + "\n" + ep.pub_date_raw;
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    static const char* hex = "0123456789abcdef";
    std::string out = "h:";
    for (int i = 60; i >= 0; i -= 4) out += hex[(h >> i) & 0xF];
    return out;
}

namespace pf_detail {

// ── Locate the first <tag …> element at or after `from`. ─────────────────────
// Matches only a real element open: the char after "<tag" must be '>', '/', or
// whitespace, so <item> is found but <itunes:*> / <itemx> are not. On success
// sets the '<' position, the open-tag '>' position, self-closing flag, the body
// span [body_begin, body_end), and `after` = index just past the element. The
// close scan assumes these tags do not nest (true for feed elements).
inline bool findElement(const std::string& s, const std::string& tag, size_t from,
                        size_t& open_lt, size_t& open_gt, bool& self_closing,
                        size_t& body_begin, size_t& body_end, size_t& after) {
    const std::string needle = "<" + tag;
    size_t p = from;
    for (;;) {
        p = s.find(needle, p);
        if (p == std::string::npos) return false;
        size_t nx = p + needle.size();
        char c = (nx < s.size()) ? s[nx] : '\0';
        if (c == '>' || c == '/' || c == ' ' || c == '\t' || c == '\n' || c == '\r')
            break;
        p = nx;  // a longer name that merely starts with `tag` — keep looking
    }
    open_lt = p;
    size_t gt = s.find('>', p);
    if (gt == std::string::npos) return false;
    open_gt = gt;
    self_closing = (gt > 0 && s[gt - 1] == '/');
    if (self_closing) { body_begin = body_end = gt; after = gt + 1; return true; }
    const std::string close = "</" + tag + ">";
    size_t c = s.find(close, gt + 1);
    if (c == std::string::npos) {           // truncated / unclosed — take the rest, don't fail
        body_begin = gt + 1; body_end = s.size(); after = s.size(); return true;
    }
    body_begin = gt + 1; body_end = c; after = c + close.size();
    return true;
}

// Raw body of the first <tag>…</tag> in s ("" if none). Attributes on the open
// tag are tolerated; the body is returned verbatim (caller cleans it).
inline std::string elementBody(const std::string& s, const std::string& tag) {
    size_t lt, gt, bb, be, af; bool sc;
    if (!findElement(s, tag, 0, lt, gt, sc, bb, be, af)) return "";
    return s.substr(bb, be - bb);
}

// s with the first <tag …>…</tag> (or self-closing <tag …/>) removed. Used to
// drop the channel's nested <image> block before reading the channel title/link
// (that sub-block carries its own <title>/<link>).
inline std::string removeElement(const std::string& s, const std::string& tag) {
    size_t lt, gt, bb, be, af; bool sc;
    if (!findElement(s, tag, 0, lt, gt, sc, bb, be, af)) return s;
    return s.substr(0, lt) + s.substr(af);
}

// Read an attribute value from an element's open-tag substring. `attr` must sit
// on a token boundary (preceded by start/whitespace) so "url" does not match
// inside "rss_url"; the value may be single- or double-quoted.
inline std::string attrValue(const std::string& openTag, const std::string& attr) {
    size_t p = 0;
    while ((p = openTag.find(attr, p)) != std::string::npos) {
        bool pre_ok = (p == 0) || std::isspace((unsigned char)openTag[p - 1]);
        size_t q = p + attr.size();
        size_t r = q;
        while (r < openTag.size() && std::isspace((unsigned char)openTag[r])) ++r;
        if (pre_ok && r < openTag.size() && openTag[r] == '=') {
            ++r;
            while (r < openTag.size() && std::isspace((unsigned char)openTag[r])) ++r;
            if (r < openTag.size() && (openTag[r] == '"' || openTag[r] == '\'')) {
                char quote = openTag[r++];
                size_t e = openTag.find(quote, r);
                if (e == std::string::npos) return "";
                return openTag.substr(r, e - r);
            }
        }
        p = q;
    }
    return "";
}

// Value of `attr` on the first <tag …> element in s ("" if the element or the
// attribute is absent). Works for self-closing elements (enclosure, itunes:image).
inline std::string firstAttr(const std::string& s, const std::string& tag,
                             const std::string& attr) {
    size_t lt, gt, bb, be, af; bool sc;
    if (!findElement(s, tag, 0, lt, gt, sc, bb, be, af)) return "";
    return attrValue(s.substr(lt, gt - lt + 1), attr);
}

inline const std::string& firstNonEmpty(const std::string& a, const std::string& b) {
    return a.empty() ? b : a;
}

// Append a Unicode code point as UTF-8 (numeric-entity decode).
inline void appendUtf8(std::string& o, unsigned long cp) {
    if (cp <= 0x7F) {
        o += (char)cp;
    } else if (cp <= 0x7FF) {
        o += (char)(0xC0 | (cp >> 6));
        o += (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        o += (char)(0xE0 | (cp >> 12));
        o += (char)(0x80 | ((cp >> 6) & 0x3F));
        o += (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0x10FFFF) {
        o += (char)(0xF0 | (cp >> 18));
        o += (char)(0x80 | ((cp >> 12) & 0x3F));
        o += (char)(0x80 | ((cp >> 6) & 0x3F));
        o += (char)(0x80 | (cp & 0x3F));
    }
}

// XML entity unescape: the five named entities plus numeric &#NNN; / &#xHH;.
// A DUPLICATE of PlaylistManager's xml_unescape, extended for numeric forms
// (feeds use &#39; &#8217; heavily). Digits parsed by hand so a bad entity can
// never throw — an unrecognized "&…;" is left verbatim.
inline std::string unescapeEntities(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '&') {
            size_t sc = s.find(';', i + 1);
            if (sc != std::string::npos && sc - i <= 12) {
                std::string ent = s.substr(i + 1, sc - i - 1);
                bool matched = false;
                std::string rep;
                if (ent == "amp")       { rep = "&";  matched = true; }
                else if (ent == "lt")   { rep = "<";  matched = true; }
                else if (ent == "gt")   { rep = ">";  matched = true; }
                else if (ent == "quot") { rep = "\""; matched = true; }
                else if (ent == "apos") { rep = "'";  matched = true; }
                else if (ent.size() >= 2 && ent[0] == '#') {
                    unsigned long cp = 0; bool ok = false;
                    if (ent[1] == 'x' || ent[1] == 'X') {
                        for (size_t k = 2; k < ent.size(); ++k) {
                            char c = ent[k];
                            unsigned d;
                            if (c >= '0' && c <= '9') d = c - '0';
                            else if (c >= 'a' && c <= 'f') d = 10 + (c - 'a');
                            else if (c >= 'A' && c <= 'F') d = 10 + (c - 'A');
                            else { ok = false; break; }
                            cp = cp * 16 + d; ok = true;
                        }
                    } else {
                        for (size_t k = 1; k < ent.size(); ++k) {
                            char c = ent[k];
                            if (c < '0' || c > '9') { ok = false; break; }
                            cp = cp * 10 + (c - '0'); ok = true;
                        }
                    }
                    if (ok && cp > 0 && cp <= 0x10FFFF) { appendUtf8(rep, cp); matched = true; }
                }
                if (matched) { out += rep; i = sc + 1; continue; }
            }
        }
        out += s[i++];
    }
    return out;
}

// Replace each <![CDATA[ … ]]> span with its literal content; text outside CDATA
// passes through. A truncated CDATA (no closing ]]>) takes the remainder.
inline std::string unwrapCData(const std::string& s) {
    static const std::string open = "<![CDATA[", close = "]]>";
    std::string out;
    out.reserve(s.size());
    size_t p = 0;
    while (p < s.size()) {
        size_t a = s.find(open, p);
        if (a == std::string::npos) { out += s.substr(p); break; }
        out += s.substr(p, a - p);
        size_t b = s.find(close, a + open.size());
        if (b == std::string::npos) { out += s.substr(a + open.size()); break; }
        out += s.substr(a + open.size(), b - (a + open.size()));
        p = b + close.size();
    }
    return out;
}

// Best-effort HTML tag strip: each <…> becomes a space (so words don't fuse),
// everything else passes. Descriptions only; titles rarely carry markup.
inline std::string stripHtml(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool in_tag = false;
    for (char c : s) {
        if (c == '<')      { in_tag = true;  out += ' '; }
        else if (c == '>') { in_tag = false; }
        else if (!in_tag)  { out += c; }
    }
    return out;
}

// Trim + collapse internal whitespace runs to single spaces.
inline std::string collapseWs(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool pending = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            pending = true;
        } else {
            if (pending && !out.empty()) out += ' ';
            pending = false;
            out += c;
        }
    }
    return out;
}

// Title/plain-text pipeline: unwrap CDATA, unescape entities, collapse whitespace.
inline std::string cleanText(const std::string& raw) {
    return collapseWs(unescapeEntities(unwrapCData(raw)));
}

// Description pipeline: as cleanText but strip HTML too. Unescape runs before the
// strip so entity-encoded markup (&lt;p&gt;) is also removed.
inline std::string cleanRich(const std::string& raw) {
    return collapseWs(stripHtml(unescapeEntities(unwrapCData(raw))));
}

// <itunes:duration> -> seconds. Accepts bare seconds ("3600"), MM:SS ("58:30"),
// and HH:MM:SS ("1:02:30"). Anything unparseable -> 0. Never throws.
inline std::int64_t parseDuration(const std::string& in) {
    std::string s = collapseWs(in);
    if (s.empty()) return 0;
    // Split on ':'
    std::int64_t parts[4] = {0, 0, 0, 0};
    int n = 0;
    std::int64_t cur = 0; bool any = false;
    for (size_t i = 0; i <= s.size(); ++i) {
        char c = (i < s.size()) ? s[i] : ':';   // sentinel flush
        if (c == ':') {
            if (n >= 4) return 0;
            parts[n++] = cur; cur = 0;
            if (i == s.size()) break;
        } else if (c >= '0' && c <= '9') {
            cur = cur * 10 + (c - '0'); any = true;
        } else if (c == '.' ) {
            // fractional seconds — ignore the remainder of this component
            // by consuming digits until the next ':' or end
            while (i + 1 < s.size() && s[i + 1] != ':') ++i;
        } else {
            return 0;  // stray non-numeric -> unparseable
        }
    }
    if (!any) return 0;
    switch (n) {
        case 1: return parts[0];                                   // SS
        case 2: return parts[0] * 60 + parts[1];                   // MM:SS
        case 3: return parts[0] * 3600 + parts[1] * 60 + parts[2]; // HH:MM:SS
        default: return 0;
    }
}

// Days since 1970-01-01 for a proleptic-Gregorian date (Howard Hinnant's
// algorithm) — pure integer math, no libc timezone calls, no throw.
inline std::int64_t daysFromCivil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (std::int64_t)doe - 719468;
}

// RFC-822-ish <pubDate> -> epoch seconds, best-effort. Handles the optional
// "Dow, " prefix, named months, 2- or 4-digit years, HH:MM(:SS), and both
// numeric (+/-HHMM) and named (GMT/UT/UTC/EST…PDT) zones. 0 on any failure —
// the raw string is always kept by the caller regardless.
inline std::int64_t parseRfc822(const std::string& in) {
    std::string s = collapseWs(in);
    size_t comma = s.find(',');
    if (comma != std::string::npos && comma <= 4) s = s.substr(comma + 1);
    // tokenize on spaces
    std::vector<std::string> t;
    {
        size_t i = 0;
        while (i < s.size()) {
            while (i < s.size() && s[i] == ' ') ++i;
            size_t j = i;
            while (j < s.size() && s[j] != ' ') ++j;
            if (j > i) t.push_back(s.substr(i, j - i));
            i = j;
        }
    }
    if (t.size() < 4) return 0;   // need day month year time [zone]
    auto toInt = [](const std::string& x, bool& ok) -> long {
        long v = 0; ok = !x.empty();
        for (char c : x) { if (c < '0' || c > '9') { ok = false; break; } v = v * 10 + (c - '0'); }
        return v;
    };
    bool ok = false;
    long day = toInt(t[0], ok); if (!ok || day < 1 || day > 31) return 0;
    static const char* mon[12] = {"Jan","Feb","Mar","Apr","May","Jun",
                                  "Jul","Aug","Sep","Oct","Nov","Dec"};
    unsigned month = 0;
    for (unsigned k = 0; k < 12; ++k)
        if (t[1].size() >= 3 &&
            std::tolower((unsigned char)t[1][0]) == std::tolower((unsigned char)mon[k][0]) &&
            std::tolower((unsigned char)t[1][1]) == std::tolower((unsigned char)mon[k][1]) &&
            std::tolower((unsigned char)t[1][2]) == std::tolower((unsigned char)mon[k][2])) {
            month = k + 1; break;
        }
    if (!month) return 0;
    long year = toInt(t[2], ok); if (!ok) return 0;
    if (year < 100) year += (year < 70) ? 2000 : 1900;   // RFC-822 2-digit year

    // time HH:MM[:SS]
    long hh = 0, mm = 0, ssec = 0;
    {
        const std::string& tm = t[3];
        long v[3] = {0, 0, 0}; int nc = 0; long cur = 0; bool anyd = false; bool good = true;
        for (size_t i = 0; i <= tm.size(); ++i) {
            char c = (i < tm.size()) ? tm[i] : ':';
            if (c == ':') { if (nc >= 3) { good = false; break; } v[nc++] = cur; cur = 0; if (i == tm.size()) break; }
            else if (c >= '0' && c <= '9') { cur = cur * 10 + (c - '0'); anyd = true; }
            else { good = false; break; }
        }
        if (!good || !anyd) return 0;
        hh = v[0]; mm = v[1]; ssec = v[2];
    }
    if (hh > 23 || mm > 59 || ssec > 60) return 0;

    // zone offset (seconds to SUBTRACT to reach UTC)
    long off = 0;
    if (t.size() >= 5) {
        const std::string& z = t[4];
        if (!z.empty() && (z[0] == '+' || z[0] == '-') && z.size() >= 5) {
            bool zk = true;
            long zh = toInt(z.substr(1, 2), zk);
            long zm = zk ? toInt(z.substr(3, 2), zk) : 0;
            if (zk) off = (z[0] == '-' ? -1 : 1) * (zh * 3600 + zm * 60);
        } else {
            struct { const char* n; long h; } named[] = {
                {"GMT",0},{"UT",0},{"UTC",0},{"Z",0},
                {"EST",-5},{"EDT",-4},{"CST",-6},{"CDT",-5},
                {"MST",-7},{"MDT",-6},{"PST",-8},{"PDT",-7},
            };
            for (auto& e : named) if (z == e.n) { off = e.h * 3600; break; }
        }
    }

    std::int64_t days = daysFromCivil(year, month, (unsigned)day);
    return days * 86400 + hh * 3600 + mm * 60 + ssec - off;
}

}  // namespace pf_detail

// ── Entry point ──────────────────────────────────────────────────────────────
inline PodcastFeed parsePodcastFeed(const std::string& xml) {
    using namespace pf_detail;
    PodcastFeed feed;

    size_t lt, gt, bb, be, af; bool sc;
    if (!findElement(xml, "channel", 0, lt, gt, sc, bb, be, af))
        return feed;                                   // no RSS channel -> ok=false
    std::string channel = xml.substr(bb, be - bb);

    // Channel metadata comes from the head region (before the first <item>).
    size_t item_pos = channel.size();
    {
        size_t l2, g2, b2, e2, a2; bool s2;
        if (findElement(channel, "item", 0, l2, g2, s2, b2, e2, a2)) item_pos = l2;
    }
    std::string head = channel.substr(0, item_pos);
    std::string head_noimg = removeElement(head, "image");   // drop nested <image> title/link

    feed.title       = cleanText(elementBody(head_noimg, "title"));
    feed.link        = cleanText(elementBody(head_noimg, "link"));
    feed.author      = cleanText(firstNonEmpty(elementBody(head, "itunes:author"),
                                               elementBody(head, "managingEditor")));
    feed.description = cleanRich(firstNonEmpty(elementBody(head, "description"),
                                               elementBody(head, "itunes:summary")));
    feed.image_url   = firstNonEmpty(firstAttr(head, "itunes:image", "href"),
                                     elementBody(elementBody(head, "image"), "url"));

    // Episodes: each <item> block, in feed order.
    size_t pos = 0;
    for (;;) {
        size_t il, ig, ib, ie, ia; bool isc;
        if (!findElement(channel, "item", pos, il, ig, isc, ib, ie, ia)) break;
        std::string it = channel.substr(ib, ie - ib);
        pos = ia;

        PodcastEpisode ep;
        ep.audio_url = unescapeEntities(firstAttr(it, "enclosure", "url"));
        if (ep.audio_url.empty()) { ++feed.skipped_episodes; continue; }   // unplayable -> skip

        ep.title        = cleanText(firstNonEmpty(elementBody(it, "title"),
                                                  elementBody(it, "itunes:title")));
        ep.guid         = collapseWs(unescapeEntities(elementBody(it, "guid")));
        ep.pub_date_raw = collapseWs(elementBody(it, "pubDate"));
        ep.pub_date_unix = parseRfc822(ep.pub_date_raw);
        ep.duration_sec = parseDuration(elementBody(it, "itunes:duration"));
        ep.description  = cleanRich(firstNonEmpty(elementBody(it, "description"),
                                                  elementBody(it, "itunes:summary")));
        ep.image_url    = firstAttr(it, "itunes:image", "href");

        feed.episodes.push_back(std::move(ep));
    }

    feed.ok = !feed.episodes.empty();
    return feed;
}
