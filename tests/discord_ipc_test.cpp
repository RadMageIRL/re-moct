// discord_ipc_test — Windows-only unit test for the core::IIpc seam (slice 6):
// drives the REAL DiscordRP consumer through an injected FakeIpc (constructor
// injection — no global) and asserts the Discord protocol frames it builds:
//   - endpoint probe: discord-ipc-0..9 in order, first accepting slot wins,
//     all-refused -> silent no-op (Discord not running)
//   - handshake: opcode 0, exact {"v":1,"client_id":...} payload, drained response
//   - SET_ACTIVITY: opcode 1, pid field, details/state/timestamps/assets shape,
//     jsonEscape correctness (quotes, backslash, newline, control chars)
//   - clearActivity: "activity":null
//   - CLOSE (opcode 2) handshake response -> rejected, no activity frame sent
//   - lazy reconnect: dead channel -> failed send disconnects; the NEXT call
//     re-probes and re-handshakes on a fresh channel (the Discord-restart path)
// The framing/protocol is proven here; only live Discord proves the pipe transport.
#include "core/IIpc.h"
#include "DiscordRP.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ ++g_fail; \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);} }while(0)

// ── frame helpers ───────────────────────────────────────────────────────────
static std::string makeFrame(uint32_t op, const std::string& payload) {
    std::string f(8, '\0');
    uint32_t len = (uint32_t)payload.size();
    std::memcpy(&f[0], &op,  4);
    std::memcpy(&f[4], &len, 4);
    return f + payload;
}
static bool parseFrame(const std::string& raw, uint32_t& op, std::string& payload) {
    if (raw.size() < 8) return false;
    uint32_t len = 0;
    std::memcpy(&op,  raw.data() + 0, 4);
    std::memcpy(&len, raw.data() + 4, 4);
    if (raw.size() != 8 + len) return false;
    payload = raw.substr(8);
    return true;
}
static bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// ── fakes ───────────────────────────────────────────────────────────────────
// Channel state lives OUTSIDE the channel object (shared_ptr held by FakeIpc):
// DiscordRP destroys its channel on every failure path (that's the behavior under
// test), so the captured frames must outlive the channel or the assertions read
// freed memory — the fixture-observation twin of the http_cancel_test lesson.
struct ChanState {
    std::vector<std::string> sent;   // one entry per send() call, verbatim (the
                                     // single-write framing contract is visible here)
    std::string rx;                  // scripted inbound bytes
    std::size_t rx_pos = 0;
    bool dead = false;               // flipped by tests to simulate a dead pipe
};

struct FakeChannel final : core::IIpcChannel {
    std::shared_ptr<ChanState> s;
    explicit FakeChannel(std::shared_ptr<ChanState> st) : s(std::move(st)) {}

    bool send(const void* data, std::size_t len) override {
        if (s->dead) return false;
        s->sent.emplace_back(static_cast<const char*>(data), len);
        return true;
    }
    bool waitReadable(std::size_t min_bytes, int /*timeout_ms*/) override {
        return !s->dead && (s->rx.size() - s->rx_pos) >= min_bytes;
    }
    bool recvSome(void* out, std::size_t max_len, std::size_t& got) override {
        if (s->dead) { got = 0; return false; }
        got = s->rx.size() - s->rx_pos;
        if (got > max_len) got = max_len;
        if (got == 0) return false;          // nothing scripted = broken (never block)
        std::memcpy(out, s->rx.data() + s->rx_pos, got);
        s->rx_pos += got;
        return true;
    }
};

// A scripted endpoint table: records connect() order; slots below `accept_from`
// refuse (nullptr, like CreateFileW failing when Discord isn't on that slot).
struct FakeIpc final : core::IIpc {
    int accept_from = 0;             // first slot index that accepts (10 = refuse all)
    std::string handshake_reply;     // rx pre-loaded into every accepted channel
    std::vector<std::string> tried;  // endpoint names, in connect order
    std::vector<std::shared_ptr<ChanState>> chans;  // outlives DiscordRP's channels

    ChanState* last() { return chans.empty() ? nullptr : chans.back().get(); }

    std::unique_ptr<core::IIpcChannel> connect(const std::string& endpoint) override {
        tried.push_back(endpoint);
        int idx = endpoint.empty() ? -1 : (endpoint.back() - '0');
        if (idx < accept_from) return nullptr;
        auto st = std::make_shared<ChanState>();
        st->rx = handshake_reply;
        chans.push_back(st);
        return std::make_unique<FakeChannel>(st);
    }
};

static const std::string READY =
    makeFrame(1, R"({"cmd":"DISPATCH","evt":"READY","data":{}})");

int main() {
    // ---- (1) handshake + SET_ACTIVITY frame shapes through a clean connect ----
    {
        FakeIpc ipc;
        ipc.handshake_reply = READY + READY;   // one drained per write we make
        DiscordRP rp("123456789", &ipc);
        rp.setActivity("Jóga", "Björk — Homogenic", 1751400000,
                       "https://img.example/cover.jpg", "Homogenic");

        CHECK(ipc.tried.size() == 1 && ipc.tried[0] == "discord-ipc-0");
        CHECK(ipc.last() != nullptr && ipc.last()->sent.size() == 2);

        uint32_t op = 99; std::string payload;
        CHECK(parseFrame(ipc.last()->sent[0], op, payload));  // one send() per frame:
        CHECK(op == 0);                                     // the single-write contract
        CHECK(payload == "{\"v\":1,\"client_id\":\"123456789\"}");

        CHECK(parseFrame(ipc.last()->sent[1], op, payload));
        CHECK(op == 1);
        CHECK(contains(payload, "\"cmd\":\"SET_ACTIVITY\""));
        CHECK(contains(payload, "\"pid\":"));
        CHECK(contains(payload, "\"details\":\"Jóga\""));           // UTF-8 untouched
        CHECK(contains(payload, "\"state\":\"Björk — Homogenic\""));
        CHECK(contains(payload, "\"timestamps\":{\"start\":1751400000}"));
        CHECK(contains(payload, "\"large_image\":\"https://img.example/cover.jpg\""));
        CHECK(contains(payload, "\"large_text\":\"Homogenic\""));
        CHECK(contains(payload, "\"nonce\":\""));
        CHECK(rp.connected());
    }

    // ---- (2) endpoint probing: refuse 0..3, accept 4 ----
    {
        FakeIpc ipc;
        ipc.accept_from = 4;
        ipc.handshake_reply = READY + READY;
        DiscordRP rp("42", &ipc);
        rp.setActivity("t", "s", 0, "", "");
        CHECK(ipc.tried.size() == 5);
        CHECK(ipc.tried[0] == "discord-ipc-0" && ipc.tried[4] == "discord-ipc-4");
        CHECK(rp.connected());
        // start_epoch == 0 omits the timer; empty image omits assets
        uint32_t op; std::string payload;
        CHECK(parseFrame(ipc.last()->sent[1], op, payload));
        CHECK(!contains(payload, "timestamps"));
        CHECK(!contains(payload, "assets"));
    }

    // ---- (3) Discord not running: all slots refuse -> silent no-op ----
    {
        FakeIpc ipc;
        ipc.accept_from = 10;
        DiscordRP rp("42", &ipc);
        rp.setActivity("t", "s", 0, "", "");
        CHECK(ipc.tried.size() == 10);                 // probed 0..9, gave up
        CHECK(!rp.connected());
        rp.clearActivity();                            // no connection: must no-op
        CHECK(ipc.tried.size() == 10);                 // ...without reconnecting
    }

    // ---- (4) CLOSE handshake response -> rejected, nothing further sent ----
    {
        FakeIpc ipc;
        ipc.handshake_reply = makeFrame(2, R"({"code":4000,"message":"not logged in"})");
        DiscordRP rp("42", &ipc);
        rp.setActivity("t", "s", 0, "", "");
        CHECK(!rp.connected());
        CHECK(ipc.last() != nullptr && ipc.last()->sent.size() == 1);   // handshake only
    }

    // ---- (5) jsonEscape through the real path ----
    {
        FakeIpc ipc;
        ipc.handshake_reply = READY + READY;
        DiscordRP rp("42", &ipc);
        rp.setActivity("say \"hi\"\\now", "tab\there\nline", 0, "", "");
        uint32_t op; std::string payload;
        CHECK(parseFrame(ipc.last()->sent[1], op, payload));
        CHECK(contains(payload, "\"details\":\"say \\\"hi\\\"\\\\now\""));
        CHECK(contains(payload, "\"state\":\"tab\\there\\nline\""));
        std::string ctrl(1, '\x01');
        rp.disconnect();
        // control char -> \u00xx (fresh object; the fake refills rx per connect)
        DiscordRP rp2("42", &ipc);
        rp2.setActivity(ctrl, "", 0, "", "");
        CHECK(parseFrame(ipc.last()->sent[1], op, payload));
        CHECK(contains(payload, "\"details\":\"\\u0001\""));
    }

    // ---- (6) clearActivity keeps the connection, sends activity:null ----
    {
        FakeIpc ipc;
        ipc.handshake_reply = READY + READY + READY;
        DiscordRP rp("42", &ipc);
        rp.setActivity("t", "s", 0, "", "");
        rp.clearActivity();
        CHECK(ipc.last()->sent.size() == 3);
        uint32_t op; std::string payload;
        CHECK(parseFrame(ipc.last()->sent[2], op, payload));
        CHECK(op == 1);
        CHECK(contains(payload, "\"activity\":null"));
        CHECK(rp.connected());
    }

    // ---- (7) lazy reconnect after channel death (the Discord-restart path) ----
    {
        FakeIpc ipc;
        ipc.handshake_reply = READY + READY;
        DiscordRP rp("42", &ipc);
        rp.setActivity("t", "s", 0, "", "");
        CHECK(rp.connected());
        CHECK(ipc.tried.size() == 1);

        ipc.last()->dead = true;                 // Discord quit: pipe breaks
        rp.setActivity("t2", "s2", 0, "", ""); // send fails -> disconnects, no crash
        CHECK(!rp.connected());

        rp.setActivity("t3", "s3", 0, "", ""); // NEXT call re-probes + re-handshakes
        CHECK(rp.connected());
        CHECK(ipc.tried.size() == 2);          // a second connect happened
        uint32_t op; std::string payload;
        CHECK(ipc.last()->sent.size() == 2);     // fresh channel: handshake + activity
        CHECK(parseFrame(ipc.last()->sent[0], op, payload));
        CHECK(op == 0);                        // re-handshake, not a bare resume
        CHECK(parseFrame(ipc.last()->sent[1], op, payload));
        CHECK(contains(payload, "\"details\":\"t3\""));
    }

    if (g_fail) { std::printf("%d check(s) FAILED\n", g_fail); return 1; }
    std::printf("discord_ipc_test: all checks passed\n");
    return 0;
}
