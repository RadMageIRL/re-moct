#include "Toast.h"
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "StringUtils.h"   // utf8_to_wide

// Base64-encode raw bytes (standard alphabet, '=' padded). Used to build a
// PowerShell -EncodedCommand argument: the payload is base64 of the UTF-16LE
// script, so there is no outer command-line quoting for untrusted track
// metadata to break out of (the old -Command "..." form could be terminated
// early by a stray double-quote in a title/artist/album).
static std::string b64encode(const unsigned char* data, size_t len) {
    static const char* T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        unsigned v = (data[i] << 16) | (data[i+1] << 8) | data[i+2];
        out += T[(v >> 18) & 0x3F]; out += T[(v >> 12) & 0x3F];
        out += T[(v >>  6) & 0x3F]; out += T[v & 0x3F];
    }
    if (i < len) {
        bool two = (i + 1 < len);
        unsigned v = (data[i] << 16) | (two ? (data[i+1] << 8) : 0u);
        out += T[(v >> 18) & 0x3F];
        out += T[(v >> 12) & 0x3F];
        out += two ? T[(v >> 6) & 0x3F] : '=';
        out += '=';
    }
    return out;
}
#endif

void showTrackToast(const std::string& title,
                    const std::string& artist,
                    const std::string& album) {
#ifdef _WIN32
    // Build display strings
    std::string top  = artist.empty() ? title : artist + " - " + title;
    std::string body = album.empty()  ? "RE-MOCT" : album;

    // Escape single quotes for PowerShell
    auto esc = [](std::string s) {
        std::string out;
        for (char c : s) {
            if (c == '\'') out += "''";
            else           out += c;
        }
        return out;
    };

    std::string ps =
        "[Console]::OutputEncoding=[System.Text.Encoding]::UTF8;"
        "[Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType=WindowsRuntime] | Out-Null;"
        "[Windows.Data.Xml.Dom.XmlDocument, Windows.Data.Xml.Dom.XmlDocument, ContentType=WindowsRuntime] | Out-Null;"
        "$template = [Windows.UI.Notifications.ToastTemplateType]::ToastImageAndText02;"
        "$xml = [Windows.UI.Notifications.ToastNotificationManager]::GetTemplateContent($template);"
        "$text = $xml.GetElementsByTagName('text');"
        "$text[0].AppendChild($xml.CreateTextNode('" + esc(top)  + "')) | Out-Null;"
        "$text[1].AppendChild($xml.CreateTextNode('" + esc(body) + "')) | Out-Null;"
        "$toast = [Windows.UI.Notifications.ToastNotification]::new($xml);"
        "[Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier('RE-MOCT').Show($toast);";

    // Encode the script as base64 of its UTF-16LE bytes and hand it to
    // -EncodedCommand. With no outer quoting, a double-quote or CR/LF in
    // metadata (which can arrive from network stream tags) cannot break or
    // inject into the command line. esc() still doubles single quotes so the
    // PS single-quoted string literals around the metadata stay intact.
    std::wstring wps = utf8_to_wide(ps);
    std::string  b64 = b64encode(
        reinterpret_cast<const unsigned char*>(wps.data()),
        wps.size() * sizeof(wchar_t));
    std::string cmd = "powershell -NoProfile -NonInteractive -EncodedCommand " + b64;

    // Fire and forget in a detached thread — don't block the audio/UI threads
    std::thread([cmd]() {
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        std::string c = cmd;  // need mutable copy
        CreateProcessA(nullptr, c.data(), nullptr, nullptr,
                       FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        if (pi.hProcess) {
            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    }).detach();
#else
    (void)title; (void)artist; (void)album;
#endif
}
