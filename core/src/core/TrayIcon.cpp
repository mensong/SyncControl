#include "core/TrayIcon.h"

#include <shellapi.h>

namespace synccontrol {
namespace {

constexpr UINT kCallbackMessage = WM_APP + 1;
constexpr UINT kExitCommand = 1;
TrayIcon* g_instance = nullptr;

LRESULT showContextMenu(HWND window) {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return 0;
    }
    AppendMenuW(menu, MF_STRING, kExitCommand, L"退出");

    POINT position{};
    GetCursorPos(&position);
    SetForegroundWindow(window);
    const UINT command = TrackPopupMenu(
        menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        position.x, position.y, 0, window, nullptr);
    DestroyMenu(menu);
    if (command == kExitCommand) {
        PostQuitMessage(0);
    }
    return 0;
}

}  // namespace

TrayIcon::~TrayIcon() {
    stop();
}

bool TrayIcon::start(HINSTANCE instance, const wchar_t* tooltip) {
    if (m_window != nullptr) {
        return true;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &TrayIcon::windowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"SyncControlCoreTrayWindow";
    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    m_window = CreateWindowExW(
        0, windowClass.lpszClassName, L"SyncControl Core", WS_OVERLAPPED,
        CW_USEDEFAULT, CW_USEDEFAULT, 0, 0, nullptr, nullptr, instance, nullptr);
    if (m_window == nullptr) {
        return false;
    }

    m_tooltip = tooltip != nullptr ? tooltip : L"SyncControl Core";
    m_icon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    m_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    g_instance = this;
    return add();
}

bool TrayIcon::add() {
    m_data = {};
    m_data.cbSize = sizeof(m_data);
    m_data.hWnd = m_window;
    m_data.uID = 1;
    m_data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    m_data.uCallbackMessage = kCallbackMessage;
    m_data.hIcon = m_icon;
    if (wcscpy_s(m_data.szTip, m_tooltip.c_str()) != 0) {
        return false;
    }
    return Shell_NotifyIconW(NIM_ADD, &m_data) != FALSE;
}

void TrayIcon::stop() {
    if (m_window != nullptr) {
        Shell_NotifyIconW(NIM_DELETE, &m_data);
        DestroyWindow(m_window);
        m_window = nullptr;
    }
    if (g_instance == this) {
        g_instance = nullptr;
    }
}

LRESULT CALLBACK TrayIcon::windowProc(HWND window, UINT message,
                                      WPARAM wParam, LPARAM lParam) {
    if (g_instance != nullptr && message == g_instance->m_taskbarCreatedMessage &&
        g_instance->m_window == window) {
        g_instance->add();
        return 0;
    }

    switch (message) {
        case kCallbackMessage:
            if (LOWORD(lParam) == WM_CONTEXTMENU || LOWORD(lParam) == WM_RBUTTONUP) {
                return showContextMenu(window);
            }
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == kExitCommand) {
                PostQuitMessage(0);
            }
            return 0;
        case WM_DESTROY:
            if (g_instance != nullptr) {
                Shell_NotifyIconW(NIM_DELETE, &g_instance->m_data);
                g_instance->m_window = nullptr;
            }
            return 0;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
    }
}

}  // namespace synccontrol
