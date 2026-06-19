#include "Toast.h"
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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

    std::string cmd = "powershell -WindowStyle Hidden -NonInteractive -Command \"" + ps + "\"";

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
