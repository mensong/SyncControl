#include "core/ConfirmWindow.h"

#include <windowsx.h>

#include "core/Logger.h"

namespace synccontrol {
namespace {

constexpr wchar_t kClassName[] = L"SyncControlConfirmWindow";
constexpr int kAcceptCommand = 1101;
constexpr int kIgnoreCommand = 1102;
constexpr size_t kMaxPreviewNames = 8;
constexpr UINT kShowMessage = WM_APP + 201;
constexpr UINT kHideMessage = WM_APP + 202;
constexpr UINT kFrontMessage = WM_APP + 203;
constexpr int kWindowWidth = 440;
constexpr int kWindowHeight = 260;
constexpr DWORD kWindowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
constexpr DWORD kWindowExStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST;

SIZE outerWindowSize() {
    RECT frame{0, 0, kWindowWidth, kWindowHeight};
    AdjustWindowRectEx(&frame, kWindowStyle, FALSE, kWindowExStyle);
    return {frame.right - frame.left, frame.bottom - frame.top};
}

std::wstring utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                         static_cast<int>(utf8.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        wide.data(), size);
    return wide;
}

std::wstring formatBytes(uint64_t bytes) {
    const wchar_t* unit = L"B";
    double value = static_cast<double>(bytes);
    if (value >= 1024.0) {
        value /= 1024.0;
        unit = L"KB";
    }
    if (value >= 1024.0) {
        value /= 1024.0;
        unit = L"MB";
    }
    if (value >= 1024.0) {
        value /= 1024.0;
        unit = L"GB";
    }
    wchar_t buffer[64]{};
    swprintf_s(buffer, value >= 10.0 ? L"%.0f %s" : L"%.1f %s", value, unit);
    return buffer;
}

}  // namespace

ConfirmWindow* ConfirmWindow::s_instance = nullptr;

ConfirmWindow::~ConfirmWindow() {
    if (m_thread.joinable()) {
        if (m_running.load() && m_threadId != 0) {
            PostThreadMessageW(m_threadId, WM_QUIT, 0, 0);
        }
        m_thread.join();
    }
}

void ConfirmWindow::setCallbacks(ResponseFn onAccept, ResponseFn onIgnore) {
    m_onAccept = std::move(onAccept);
    m_onIgnore = std::move(onIgnore);
}

void ConfirmWindow::ensureCreated() {
    if (m_thread.joinable()) {
        if (m_running.load()) {
            return;
        }
        m_thread.join();
    }

    m_ready.store(false);
    m_running.store(false);
    try {
        m_thread = std::thread(&ConfirmWindow::uiThreadProc, this);
    } catch (const std::system_error&) {
        Logger::write("confirm window: failed to start ui thread");
        return;
    }
    m_threadId = GetThreadId(m_thread.native_handle());

    std::unique_lock<std::mutex> lock(m_stateMutex);
    m_stateCv.wait(lock, [this]() {
        return m_ready.load();
    });
}

void ConfirmWindow::show(const std::string& source, size_t fileCount,
                         uint64_t totalBytes,
                         const std::vector<std::string>& previewNames) {
    ensureCreated();
    {
        std::lock_guard<std::mutex> lock(m_contentMutex);
        m_pendingSource = source;
        m_pendingCount = fileCount;
        m_pendingBytes = totalBytes;
        m_pendingNames = previewNames;
    }
    if (m_window == nullptr || !PostMessageW(m_window, kShowMessage, 0, 0)) {
        Logger::write("confirm window: show message unavailable");
    }
}

void ConfirmWindow::hide() {
    if (m_window != nullptr) {
        PostMessageW(m_window, kHideMessage, 0, 0);
    }
}

void ConfirmWindow::bringToFront() {
    if (m_window != nullptr && m_hasTask.load()) {
        PostMessageW(m_window, kFrontMessage, 0, 0);
    }
}

bool ConfirmWindow::isVisible() const {
    return m_window != nullptr && m_hasTask.load() && IsWindowVisible(m_window);
}

DWORD WINAPI ConfirmWindow::uiThreadProc(LPVOID context) {
    auto* self = static_cast<ConfirmWindow*>(context);
    self->createWindow();
    {
        std::lock_guard<std::mutex> lock(self->m_stateMutex);
        self->m_running.store(true);
        self->m_ready.store(true);
    }
    self->m_stateCv.notify_all();

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    self->hideNow();
    if (self->m_window != nullptr) {
        DestroyWindow(self->m_window);
    }
    if (self->m_classAtom != 0) {
        UnregisterClassW(MAKEINTRESOURCEW(self->m_classAtom), GetModuleHandleW(nullptr));
        self->m_classAtom = 0;
    }
    if (s_instance == self) {
        s_instance = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(self->m_stateMutex);
        self->m_running.store(false);
    }
    return 0;
}

void ConfirmWindow::createWindow() {
    if (m_window != nullptr) {
        return;
    }

    s_instance = this;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = &ConfirmWindow::windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kClassName;
    m_classAtom = RegisterClassW(&windowClass);
    if (m_classAtom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Logger::write("confirm window: register class failed, win32Error=" +
                      std::to_string(static_cast<uint32_t>(GetLastError())));
        return;
    }

    const LPCWSTR className = m_classAtom != 0
                                  ? MAKEINTRESOURCEW(m_classAtom)
                                  : kClassName;
    const SIZE outerSize = outerWindowSize();
    m_window = CreateWindowExW(kWindowExStyle, className,
                               L"SyncControl", kWindowStyle,
                               CW_USEDEFAULT, CW_USEDEFAULT, outerSize.cx,
                               outerSize.cy, nullptr,
                               nullptr, instance, nullptr);
    if (m_window == nullptr) {
        Logger::write("confirm window: create failed, win32Error=" +
                      std::to_string(static_cast<uint32_t>(GetLastError())));
        return;
    }
    m_summary = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                16, 14, 392, 44, m_window,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(1)),
                                instance, nullptr);
    m_names = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_READONLY | ES_MULTILINE |
            ES_AUTOVSCROLL,
        16, 66, 392, 116, m_window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(2)), instance, nullptr);
    m_acceptButton = CreateWindowExW(0, L"BUTTON", L"接收",
                                     WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                     190, 192, 104, 32, m_window,
                                     reinterpret_cast<HMENU>(
                                         static_cast<INT_PTR>(kAcceptCommand)),
                                     instance, nullptr);
    m_ignoreButton = CreateWindowExW(0, L"BUTTON", L"忽略", WS_CHILD | WS_VISIBLE,
                                     304, 192, 104, 32, m_window,
                                     reinterpret_cast<HMENU>(
                                         static_cast<INT_PTR>(kIgnoreCommand)),
                                     instance, nullptr);
}

void ConfirmWindow::showNow() {
    std::string source;
    size_t count = 0;
    uint64_t bytes = 0;
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lock(m_contentMutex);
        source = std::move(m_pendingSource);
        count = m_pendingCount;
        bytes = m_pendingBytes;
        names = std::move(m_pendingNames);
        m_pendingSource.clear();
        m_pendingNames.clear();
    }
    if (m_window == nullptr) {
        return;
    }
    updateContent(source, count, bytes, names);
    m_hasTask.store(true);
    ShowWindow(m_window, SW_SHOWNOACTIVATE);
    SetWindowPos(m_window, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void ConfirmWindow::hideNow() {
    m_hasTask.store(false);
    if (m_window != nullptr) {
        ShowWindow(m_window, SW_HIDE);
    }
}

void ConfirmWindow::bringToFrontNow() {
    if (m_window == nullptr || !m_hasTask.load()) {
        return;
    }
    ShowWindow(m_window, SW_SHOWNA);
    SetWindowPos(m_window, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void ConfirmWindow::updateContent(const std::string& source, size_t fileCount,
                                  uint64_t totalBytes,
                                  const std::vector<std::string>& names) {
    if (m_window == nullptr) {
        return;
    }
    const std::wstring summary = L"来自 " + utf8ToWide(source) + L"，共 " +
                                 std::to_wstring(fileCount) + L" 项，约 " +
                                 formatBytes(totalBytes);
    std::wstring preview;
    const size_t count = names.size() < kMaxPreviewNames ? names.size()
                                                         : kMaxPreviewNames;
    for (size_t index = 0; index < count; ++index) {
        if (index != 0) {
            preview += L"\r\n";
        }
        preview += utf8ToWide(names[index]);
    }
    if (names.size() > count) {
        preview += L"\r\n... 以及其他 " +
                   std::to_wstring(names.size() - count) + L" 项";
    }
    SetWindowTextW(m_summary, summary.c_str());
    SetWindowTextW(m_names, preview.c_str());
}

LRESULT CALLBACK ConfirmWindow::windowProc(HWND window, UINT message,
                                           WPARAM wParam, LPARAM lParam) {
    if (s_instance != nullptr) {
        return s_instance->handleMessage(window, message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT ConfirmWindow::handleMessage(HWND window, UINT message,
                                     WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case kShowMessage:
            showNow();
            return 0;
        case kHideMessage:
            hideNow();
            return 0;
        case kFrontMessage:
            bringToFrontNow();
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_COMMAND:
            if (HIWORD(wParam) == BN_CLICKED) {
                if (LOWORD(wParam) == kAcceptCommand) {
                    respond(true);
                    return 0;
                }
                if (LOWORD(wParam) == kIgnoreCommand) {
                    respond(false);
                    return 0;
                }
            }
            break;
        case WM_CLOSE:
            respond(false);
            return 0;
        case WM_DESTROY:
            m_window = nullptr;
            m_summary = nullptr;
            m_names = nullptr;
            m_acceptButton = nullptr;
            m_ignoreButton = nullptr;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void ConfirmWindow::respond(bool accepted) {
    hideNow();
    ResponseFn response = accepted ? m_onAccept : m_onIgnore;
    if (response) {
        response();
    }
}

}  // namespace synccontrol
