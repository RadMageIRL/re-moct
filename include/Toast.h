#pragma once
#include <string>

// Fire a Windows 11 toast notification for track changes.
// Runs PowerShell in a detached process — no WinRT dependency.
void showTrackToast(const std::string& title,
                    const std::string& artist,
                    const std::string& album);
