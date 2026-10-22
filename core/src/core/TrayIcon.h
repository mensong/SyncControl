#pragma once

#include <windows.h>
#include <shellapi.h>
#include <string>

namespace synccontrol {

// Non-blocking tray indicator. The owning thread must run a Win32 message
// loop; Service::run already does that.
class TrayIcon {
public:
    TrayIcon() = default;
    ~TrayIcon();
    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    bool start(HINSTANCE instance, const wchar_t* tooltip);
    void stop();

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message,
                                       WPARAM wParam, LPARAM lParam);
    bool add();

    HWND m_window = nullptr;
    HICON m_icon = nullptr;
    NOTIFYICONDATAW m_data{};
    std::wstring m_tooltip;
    UINT m_taskbarCreatedMessage = 0;
};

}  // namespace synccontrol
