#pragma once

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <string>
#include <thread>

namespace synccontrol {

// Runs on its own UI thread so modal menus (TrackPopupMenu) cannot stall the
// network/service loop. Exit requests are posted to the service thread that
// is passed to start().
class TrayIcon {
public:
    TrayIcon() = default;
    ~TrayIcon();
    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    bool start(HINSTANCE instance, const wchar_t* tooltip, DWORD serviceThreadId);
    void stop();

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message,
                                       WPARAM wParam, LPARAM lParam);
    bool add();
    void threadProc();

    HINSTANCE m_instance = nullptr;
    std::thread m_thread;
    std::atomic<DWORD> m_threadId{0};
    DWORD m_serviceThreadId = 0;
    HWND m_window = nullptr;
    HICON m_icon = nullptr;
    NOTIFYICONDATAW m_data{};
    std::wstring m_tooltip;
    UINT m_taskbarCreatedMessage = 0;
};

}  // namespace synccontrol
