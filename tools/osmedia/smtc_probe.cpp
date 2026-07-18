// smtc_probe.cpp - osmedia-phase0-probe, Probe 1 (RETAINED ARTIFACT, not production).
//
// Question: can we obtain and drive a live ISystemMediaTransportControls from a
// top-level HWND, compiling + linking under MSYS2 UCRT64 GCC (no cppwinrt, no
// WRL), and register a ButtonPressed handler - via the raw WinRT ABI?
//
// Approach: the mingw-w64 <windows.media.h> + <systemmediatransportcontrolsinterop.h>
// expose the SMTC ABI in the ABI::Windows::Media namespace with __CRT_UUID_DECL,
// so __uuidof works under GCC. We create a real top-level HWND (mirrors the
// wingui build's PDC_hWnd), get the interop factory, GetForWindow, enable the
// controls, push a DisplayUpdater update (title/artist), and register a
// hand-rolled ITypedEventHandler for ButtonPressed. No cppwinrt projection.
//
// Build: see build-smtc.sh (this dir). Run: launches, publishes "RE-MOCT probe
// - Track", then pumps messages ~20 s so a media-key / flyout press fires the
// handler (prints the button). Ctrl+C or close to exit.
//
// This is a feasibility probe: the machine-checkable result is compile + link +
// each ABI call returning S_OK at runtime. The "flyout appears / media key
// fires the handler" confirmation is interactive (a human presses a key).

#include <windows.h>
#include <roapi.h>          // RoInitialize / RoGetActivationFactory
#include <winstring.h>      // WindowsCreateString / WindowsDeleteString
#include <windows.media.h>  // ABI::Windows::Media::* (SMTC), __CRT_UUID_DECL'd
#include <systemmediatransportcontrolsinterop.h>  // ISystemMediaTransportControlsInterop
#include <cstdio>

using namespace ABI::Windows::Media;
using namespace ABI::Windows::Foundation;

#define OK(call) do { HRESULT _hr = (call); \
    std::printf("  %-42s hr=0x%08lx %s\n", #call, (unsigned long)_hr, \
                SUCCEEDED(_hr) ? "OK" : "FAIL"); \
    if (FAILED(_hr)) return _hr; } while (0)

// The ButtonPressed delegate we must implement by hand (no WRL). Its parameterized
// IID comes from the mingw typedef's __CRT_UUID_DECL, resolved via __uuidof on the
// ITypedEventHandler<SMTC*, ButtonPressedEventArgs*> specialization.
using ButtonHandler = ITypedEventHandler<
    SystemMediaTransportControls*, SystemMediaTransportControlsButtonPressedEventArgs*>;

struct ButtonSink : ButtonHandler {
    LONG ref = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAgileObject) ||
            riid == __uuidof(ButtonHandler)) {
            *ppv = static_cast<ButtonHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef()  override { return (ULONG)InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&ref);
        if (r == 0) delete this;
        return (ULONG)r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(
        ISystemMediaTransportControls* /*sender*/,
        ISystemMediaTransportControlsButtonPressedEventArgs* args) override {
        SystemMediaTransportControlsButton btn = SystemMediaTransportControlsButton_Play;
        if (args) args->get_Button(&btn);
        const char* name = "?";
        switch (btn) {
            case SystemMediaTransportControlsButton_Play:     name = "Play";     break;
            case SystemMediaTransportControlsButton_Pause:    name = "Pause";    break;
            case SystemMediaTransportControlsButton_Next:     name = "Next";     break;
            case SystemMediaTransportControlsButton_Previous: name = "Previous"; break;
            case SystemMediaTransportControlsButton_Stop:     name = "Stop";     break;
            default: break;
        }
        std::printf("  >>> ButtonPressed: %s\n", name);
        std::fflush(stdout);
        return S_OK;
    }
};

static HSTRING hs(const wchar_t* s) {
    HSTRING h = nullptr;
    WindowsCreateString(s, (UINT32)wcslen(s), &h);
    return h;
}

int main() {
    std::printf("== smtc_probe (osmedia-phase0, UCRT64 GCC) ==\n");

    // A real top-level HWND, exactly the shape PDC_hWnd is in the wingui build.
    WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"RemoctSmtcProbe";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"RE-MOCT SMTC probe",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                320, 120, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { std::printf("  CreateWindow FAILED\n"); return 1; }
    ShowWindow(hwnd, SW_SHOWNA);
    std::printf("  HWND = %p\n", (void*)hwnd);

    OK(RoInitialize(RO_INIT_MULTITHREADED));

    // interop factory for Windows.Media.SystemMediaTransportControls
    HSTRING cls = hs(L"Windows.Media.SystemMediaTransportControls");
    ISystemMediaTransportControlsInterop* interop = nullptr;
    OK(RoGetActivationFactory(cls, __uuidof(ISystemMediaTransportControlsInterop),
                              (void**)&interop));
    WindowsDeleteString(cls);

    ISystemMediaTransportControls* smtc = nullptr;
    OK(interop->GetForWindow(hwnd, __uuidof(ISystemMediaTransportControls),
                             (void**)&smtc));

    OK(smtc->put_IsEnabled(TRUE));
    OK(smtc->put_IsPlayEnabled(TRUE));
    OK(smtc->put_IsPauseEnabled(TRUE));
    OK(smtc->put_IsNextEnabled(TRUE));
    OK(smtc->put_IsPreviousEnabled(TRUE));
    OK(smtc->put_PlaybackStatus(MediaPlaybackStatus_Playing));

    // Display update: Music title/artist.
    ISystemMediaTransportControlsDisplayUpdater* upd = nullptr;
    OK(smtc->get_DisplayUpdater(&upd));
    OK(upd->put_Type(MediaPlaybackType_Music));
    IMusicDisplayProperties* music = nullptr;
    OK(upd->get_MusicProperties(&music));
    HSTRING title = hs(L"RE-MOCT probe");
    HSTRING artist = hs(L"Track");
    OK(music->put_Title(title));
    OK(music->put_Artist(artist));
    OK(upd->Update());
    WindowsDeleteString(title); WindowsDeleteString(artist);

    // Register ButtonPressed - the bidirectional half (transport commands in).
    ButtonSink* sink = new ButtonSink();
    EventRegistrationToken tok{};
    OK(smtc->add_ButtonPressed(sink, &tok));
    sink->Release();   // SMTC holds its own ref

    std::printf("  published; press a media key / use the flyout (20 s)...\n");
    std::fflush(stdout);

    DWORD t0 = GetTickCount();
    MSG msg;
    while (GetTickCount() - t0 < 20000) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        Sleep(20);
    }

    smtc->remove_ButtonPressed(tok);
    if (music) music->Release();
    if (upd) upd->Release();
    if (smtc) smtc->Release();
    if (interop) interop->Release();
    RoUninitialize();
    std::printf("== done ==\n");
    return 0;
}
