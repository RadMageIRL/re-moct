// MediaControlSmtc.cpp - Windows SMTC impl of core::IMediaControl (osmedia-seam).
// Raw WinRT ABI under UCRT64 GCC (no cppwinrt/WRL), proven in
// probes/osmedia/FINDINGS.md. SMTC needs the wingui top-level HWND (PDC_hWnd),
// so the real impl is gated on REMOCT_PDCURSES; every other Windows build gets
// the no-op default. Link: -lruntimeobject -lole32 -luser32 -lgdi32.
//
// Threading: constructed + driven from the UI thread (GetForWindow + all
// updaters run there). ButtonPressed / PlaybackPositionChangeRequested fire on a
// WinRT threadpool thread - their Invoke ONLY calls the registered handler,
// which enqueues (the marshal); nothing here touches playback.
#include "core/IMediaControl.h"
#include "NoopMediaControl.h"

#ifdef REMOCT_PDCURSES

#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <asyncinfo.h>
#include <windows.foundation.h>
#include <windows.storage.streams.h>
#include <windows.media.h>
#include <systemmediatransportcontrolsinterop.h>

#include <atomic>
#include <cstring>
#include <vector>

using namespace ABI::Windows::Media;
using namespace ABI::Windows::Foundation;

extern "C" HWND PDC_hWnd;   // PDCursesMod GDI window (wingui build)

namespace {

HSTRING hs(const wchar_t* s) {
    HSTRING h = nullptr;
    WindowsCreateString(s, (UINT32)wcslen(s), &h);
    return h;
}
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}
INT64 toTicks(double seconds) {   // WinRT TimeSpan = 100 ns units
    if (seconds < 0) seconds = 0;
    return (INT64)(seconds * 10000000.0);
}

using ButtonHandler = ITypedEventHandler<
    SystemMediaTransportControls*, SystemMediaTransportControlsButtonPressedEventArgs*>;
using SeekHandler = ITypedEventHandler<
    SystemMediaTransportControls*, PlaybackPositionChangeRequestedEventArgs*>;

// One hand-rolled WinRT delegate reused for both events. IFace is the typed
// handler interface to derive from; IArgsIface is the ARGS INTERFACE its Invoke
// receives (the runtimeclass in the ITypedEventHandler<> params maps to its
// default interface). ref is a plain LONG (InterlockedIncrement wants LONG*).
template <class IFace, class IArgsIface>
struct Delegate : IFace {
    LONG ref = 1;
    std::function<void(IArgsIface*)> cb;
    explicit Delegate(std::function<void(IArgsIface*)> f) : cb(std::move(f)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAgileObject) ||
            riid == __uuidof(IFace)) { *ppv = static_cast<IFace*>(this); AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef()  override { return (ULONG)InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&ref); if (r == 0) delete this; return (ULONG)r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(ISystemMediaTransportControls*, IArgsIface* a) override {
        if (cb) cb(a);
        return S_OK;
    }
};

class SmtcMediaControl : public core::IMediaControl {
public:
    SmtcMediaControl() { init(); }

    void setCommandHandler(std::function<void(core::MediaEvent)> h) override {
        handler_ = std::move(h);
    }

    void setDefaultArt(std::vector<uint8_t> jpeg) override {
        logo_ = std::move(jpeg);   // the in-memory thumbnail is built lazily on first use
    }

    void updateNowPlaying(const core::MediaMeta& m, core::MediaStatus st,
                          double pos, double dur) override {
        if (!smtc_) return;
        smtc_->put_PlaybackStatus(mapStatus(st));
        if (updater_) {
            updater_->put_Type(MediaPlaybackType_Music);
            IMusicDisplayProperties* music = nullptr;
            if (SUCCEEDED(updater_->get_MusicProperties(&music)) && music) {
                HSTRING t = hs(widen(m.title).c_str());
                HSTRING a = hs(widen(m.artist).c_str());
                music->put_Title(t);
                music->put_Artist(a);
                WindowsDeleteString(t); WindowsDeleteString(a);
                music->Release();
            }
            // Precedence: embedded cover bytes (local file) -> URL (radio) -> logo.
            if (!m.art_bytes.empty()) setThumbnailBytes(m.art_bytes);
            else                      setThumbnail(m.art);
            updater_->Update();
        }
        pushTimeline(pos, dur);         // a real change: push the bar
        last_status_ = st;
    }

    void updatePosition(double pos, double dur, core::MediaStatus st) override {
        if (!smtc_) return;
        // Cheap: refresh status if it flipped; push the timeline at most ~1 Hz
        // (the OS interpolates the bar between). No per-tick spam.
        if (st != last_status_) { smtc_->put_PlaybackStatus(mapStatus(st)); last_status_ = st; }
        DWORD now = GetTickCount();
        if (now - last_timeline_ms_ >= 1000) pushTimeline(pos, dur);
    }

    void clear() override {
        if (!smtc_) return;
        smtc_->put_PlaybackStatus(MediaPlaybackStatus_Stopped);
        last_status_ = core::MediaStatus::Stopped;
    }

    void pump() override {}   // SMTC is COM/threadpool-driven; nothing to service

private:
    MediaPlaybackStatus mapStatus(core::MediaStatus s) {
        switch (s) {
            case core::MediaStatus::Playing: return MediaPlaybackStatus_Playing;
            case core::MediaStatus::Paused:  return MediaPlaybackStatus_Paused;
            default:                         return MediaPlaybackStatus_Stopped;
        }
    }

    void fire(core::MediaCommand c, double secs = 0.0) {
        if (handler_) handler_({c, secs});      // enqueue (thread-safe on the consumer side)
    }

    void pushTimeline(double pos, double dur) {
        if (!smtc2_ || dur <= 0.0) return;   // timeline lives on ...Controls2
        IInspectable* insp = nullptr;
        HSTRING cls = hs(L"Windows.Media.SystemMediaTransportControlsTimelineProperties");
        HRESULT hr = RoActivateInstance(cls, &insp);
        WindowsDeleteString(cls);
        if (FAILED(hr) || !insp) return;
        ISystemMediaTransportControlsTimelineProperties* tp = nullptr;
        if (SUCCEEDED(insp->QueryInterface(
                __uuidof(ISystemMediaTransportControlsTimelineProperties), (void**)&tp)) && tp) {
            ABI::Windows::Foundation::TimeSpan z{0}, p{toTicks(pos)}, e{toTicks(dur)};
            tp->put_StartTime(z);
            tp->put_MinSeekTime(z);
            tp->put_Position(p);
            tp->put_EndTime(e);
            tp->put_MaxSeekTime(e);
            smtc2_->UpdateTimelineProperties(tp);
            tp->Release();
            last_timeline_ms_ = GetTickCount();
        }
        insp->Release();
    }

    // Thumbnail: the logo floor (kMediaArtLogo -> an in-memory stream over the
    // bundled bytes) or an http(s) URL (radio art). Local-file cover art is a
    // documented follow-up. Best-effort: any failure leaves the previous
    // thumbnail untouched, never breaks title/artist.
    void setThumbnail(const std::string& art) {
        if (!updater_ || art.empty()) return;
        if (art == core::kMediaArtLogo) { setLogoThumbnail(); return; }
        if (art.rfind("http://", 0) != 0 && art.rfind("https://", 0) != 0) return;
        IUriRuntimeClassFactory* uf = nullptr;
        HSTRING ucls = hs(L"Windows.Foundation.Uri");
        HRESULT hr = RoGetActivationFactory(ucls, __uuidof(IUriRuntimeClassFactory), (void**)&uf);
        WindowsDeleteString(ucls);
        if (FAILED(hr) || !uf) return;
        IUriRuntimeClass* uri = nullptr;
        HSTRING us = hs(widen(art).c_str());
        hr = uf->CreateUri(us, &uri);
        WindowsDeleteString(us);
        uf->Release();
        if (FAILED(hr) || !uri) return;
        ABI::Windows::Storage::Streams::IRandomAccessStreamReferenceStatics* rf = nullptr;
        HSTRING rcls = hs(L"Windows.Storage.Streams.RandomAccessStreamReference");
        hr = RoGetActivationFactory(rcls,
                __uuidof(ABI::Windows::Storage::Streams::IRandomAccessStreamReferenceStatics),
                (void**)&rf);
        WindowsDeleteString(rcls);
        if (SUCCEEDED(hr) && rf) {
            ABI::Windows::Storage::Streams::IRandomAccessStreamReference* ref = nullptr;
            if (SUCCEEDED(rf->CreateFromUri(uri, &ref)) && ref) {
                updater_->put_Thumbnail(ref);
                ref->Release();
            }
            rf->Release();
        }
        uri->Release();
    }

    // Bounded poll of a WinRT async op/action to completion (no completion-
    // handler delegate). The in-memory store completes near-instantly.
    static bool awaitAsync(IInspectable* op) {
        if (!op) return false;
        ::IAsyncInfo* info = nullptr;   // asyncinfo.h: global scope, unscoped enum
        if (FAILED(op->QueryInterface(__uuidof(::IAsyncInfo), (void**)&info)) || !info)
            return false;
        AsyncStatus st = Started;
        for (int i = 0; i < 2000 && st == Started; ++i) {
            if (FAILED(info->get_Status(&st))) break;
            if (st == Started) Sleep(1);
        }
        info->Release();
        return st == Completed;
    }

    // Per-track embedded cover: build a fresh in-memory stream ref from the bytes
    // and set it, releasing the previous track ref. Not cached (changes per song).
    void setThumbnailBytes(const std::vector<uint8_t>& bytes) {
        auto* ref = buildRefFromBytes(bytes);
        if (!ref) { setLogoThumbnail(); return; }   // fall back to the logo on failure
        if (track_ref_) track_ref_->Release();
        track_ref_ = ref;
        updater_->put_Thumbnail(track_ref_);
    }

    // Build the logo thumbnail ONCE from the bytes handed via setDefaultArt.
    // Cached and reused across updates (the logo never changes).
    void setLogoThumbnail() {
        if (!logo_ref_ && !logo_.empty()) logo_ref_ = buildRefFromBytes(logo_);
        if (logo_ref_) updater_->put_Thumbnail(logo_ref_);
    }

    // JPEG/PNG bytes -> IRandomAccessStreamReference via an InMemoryRandomAccess-
    // Stream written through a DataWriter (polled StoreAsync). Caller owns the ref.
    ABI::Windows::Storage::Streams::IRandomAccessStreamReference* buildRefFromBytes(
            const std::vector<uint8_t>& bytes) {
        using namespace ABI::Windows::Storage::Streams;
        if (bytes.empty()) return nullptr;
        IInspectable* insp = nullptr;
        HSTRING cls = hs(L"Windows.Storage.Streams.InMemoryRandomAccessStream");
        HRESULT hr = RoActivateInstance(cls, &insp);
        WindowsDeleteString(cls);
        if (FAILED(hr) || !insp) return nullptr;
        IRandomAccessStream* ras = nullptr;
        insp->QueryInterface(__uuidof(IRandomAccessStream), (void**)&ras);
        insp->Release();
        if (!ras) return nullptr;

        IRandomAccessStreamReference* ref = nullptr;
        IOutputStream* os = nullptr;
        if (SUCCEEDED(ras->GetOutputStreamAt(0, &os)) && os) {
            IDataWriterFactory* dwf = nullptr;
            HSTRING dcls = hs(L"Windows.Storage.Streams.DataWriter");
            RoGetActivationFactory(dcls, __uuidof(IDataWriterFactory), (void**)&dwf);
            WindowsDeleteString(dcls);
            IDataWriter* dw = nullptr;
            if (dwf) { dwf->CreateDataWriter(os, &dw); dwf->Release(); }
            bool stored = false;
            if (dw) {
                dw->WriteBytes((UINT32)bytes.size(), (BYTE*)bytes.data());
                ABI::Windows::Foundation::IAsyncOperation<UINT32>* sop = nullptr;
                if (SUCCEEDED(dw->StoreAsync(&sop)) && sop) { stored = awaitAsync(sop); sop->Release(); }
                IOutputStream* detached = nullptr;
                dw->DetachStream(&detached);        // keep ras alive independent of dw
                if (detached) detached->Release();
                dw->Release();
            }
            os->Release();
            if (stored) {
                ras->Seek(0);
                IRandomAccessStreamReferenceStatics* rf = nullptr;
                HSTRING rcls = hs(L"Windows.Storage.Streams.RandomAccessStreamReference");
                RoGetActivationFactory(rcls, __uuidof(IRandomAccessStreamReferenceStatics), (void**)&rf);
                WindowsDeleteString(rcls);
                if (rf) { rf->CreateFromStream(ras, &ref); rf->Release(); }
            }
        }
        ras->Release();   // the stream reference holds its own ref to the stream
        return ref;
    }

    void init() {
        RoInitialize(RO_INIT_MULTITHREADED);   // idempotent-ish; ignore RPC_E_CHANGED_MODE
        ISystemMediaTransportControlsInterop* interop = nullptr;
        HSTRING cls = hs(L"Windows.Media.SystemMediaTransportControls");
        HRESULT hr = RoGetActivationFactory(cls,
                __uuidof(ISystemMediaTransportControlsInterop), (void**)&interop);
        WindowsDeleteString(cls);
        if (FAILED(hr) || !interop) return;
        if (PDC_hWnd)
            interop->GetForWindow(PDC_hWnd, __uuidof(ISystemMediaTransportControls), (void**)&smtc_);
        interop->Release();
        if (!smtc_) return;

        smtc_->put_IsEnabled(TRUE);
        smtc_->put_IsPlayEnabled(TRUE);
        smtc_->put_IsPauseEnabled(TRUE);
        smtc_->put_IsStopEnabled(TRUE);
        smtc_->put_IsNextEnabled(TRUE);
        smtc_->put_IsPreviousEnabled(TRUE);
        smtc_->get_DisplayUpdater(&updater_);
        // Timeline + position-change-requested live on ...Controls2.
        smtc_->QueryInterface(__uuidof(ISystemMediaTransportControls2), (void**)&smtc2_);

        auto* bp = new Delegate<ButtonHandler, ISystemMediaTransportControlsButtonPressedEventArgs>(
            [this](ISystemMediaTransportControlsButtonPressedEventArgs* a) {
                SystemMediaTransportControlsButton b;
                if (!a || FAILED(a->get_Button(&b))) return;
                switch (b) {
                    case SystemMediaTransportControlsButton_Play:     fire(core::MediaCommand::Play);     break;
                    case SystemMediaTransportControlsButton_Pause:    fire(core::MediaCommand::Pause);    break;
                    case SystemMediaTransportControlsButton_Stop:     fire(core::MediaCommand::Stop);     break;
                    case SystemMediaTransportControlsButton_Next:     fire(core::MediaCommand::Next);     break;
                    case SystemMediaTransportControlsButton_Previous: fire(core::MediaCommand::Previous); break;
                    default: break;
                }
            });
        EventRegistrationToken tok{};
        smtc_->add_ButtonPressed(bp, &tok);
        bp->Release();

        if (smtc2_) {
            auto* sk = new Delegate<SeekHandler, IPlaybackPositionChangeRequestedEventArgs>(
                [this](IPlaybackPositionChangeRequestedEventArgs* a) {
                    ABI::Windows::Foundation::TimeSpan ts{};
                    if (a && SUCCEEDED(a->get_RequestedPlaybackPosition(&ts)))
                        fire(core::MediaCommand::SeekToAbs, (double)ts.Duration / 10000000.0);
                });
            EventRegistrationToken tok2{};
            smtc2_->add_PlaybackPositionChangeRequested(sk, &tok2);
            sk->Release();
        }
    }

    ISystemMediaTransportControls* smtc_ = nullptr;
    ISystemMediaTransportControls2* smtc2_ = nullptr;
    ISystemMediaTransportControlsDisplayUpdater* updater_ = nullptr;
    std::vector<uint8_t> logo_;   // bundled logo bytes (setDefaultArt), for the floor
    ABI::Windows::Storage::Streams::IRandomAccessStreamReference* logo_ref_ = nullptr;  // built once
    ABI::Windows::Storage::Streams::IRandomAccessStreamReference* track_ref_ = nullptr; // per-track cover
    std::function<void(core::MediaEvent)> handler_;
    core::MediaStatus last_status_ = core::MediaStatus::Stopped;
    DWORD last_timeline_ms_ = 0;
};

} // namespace

namespace core {
IMediaControl& mediaControl() {
    static SmtcMediaControl inst;
    return inst;
}
} // namespace core

#else  // REMOCT_PDCURSES not defined: non-wingui Windows build -> no HWND, no SMTC

namespace core {
IMediaControl& mediaControl() {
    static NoopMediaControl inst;
    return inst;
}
} // namespace core

#endif
