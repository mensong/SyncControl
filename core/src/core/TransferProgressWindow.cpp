#include "core/TransferProgressWindow.h"

#include <commctrl.h>

#include <algorithm>
#include <memory>

#include "core/Logger.h"

namespace synccontrol {
namespace {

constexpr wchar_t kClassName[] = L"SyncControlTransferProgressWindow";
constexpr UINT kUpdateMessage = WM_APP + 211;
constexpr UINT kHideMessage = WM_APP + 212;
constexpr UINT kCancelButtonId = 4;
constexpr int kWindowWidth = 380;
constexpr int kWindowHeight = 148;
constexpr int kMargin = 16;
constexpr DWORD kWindowStyle = WS_POPUP | WS_CAPTION | WS_BORDER;
constexpr DWORD kWindowExStyle =
    WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE;

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

TransferProgressWindow* TransferProgressWindow::s_instance = nullptr;

TransferProgressWindow::~TransferProgressWindow() {
    if (m_thread.joinable()) {
        if (m_running.load() && m_threadId != 0) {
            PostThreadMessageW(m_threadId, WM_QUIT, 0, 0);
        }
        m_thread.join();
    }
}

void TransferProgressWindow::ensureCreated() {
    if (m_thread.joinable()) {
        if (m_running.load()) {
            return;
        }
        m_thread.join();
    }

    m_ready.store(false);
    m_running.store(false);
    try {
        m_thread = std::thread(&TransferProgressWindow::uiThreadProc, this);
    } catch (const std::system_error&) {
        Logger::write("progress window: failed to start ui thread");
        return;
    }
    m_threadId = GetThreadId(m_thread.native_handle());

    std::unique_lock<std::mutex> lock(m_stateMutex);
    m_stateCv.wait(lock, [this]() {
        return m_ready.load();
    });
}

void TransferProgressWindow::setCancelHandler(std::function<void()> handler) {
    std::lock_guard<std::mutex> lock(m_cancelHandlerMutex);
    m_cancelHandler = std::move(handler);
}

void TransferProgressWindow::update(const Snapshot& snapshot) {
    if (!snapshot.active) {
        hide();
        return;
    }
    ensureCreated();
    if (m_window == nullptr) {
        return;
    }
    bool shouldPost = false;
    {
        std::lock_guard<std::mutex> lock(m_contentMutex);
        m_pendingSnapshot = snapshot;
        shouldPost = !m_window || true;  // Post is cheap; the UI merges bursts.
    }
    if (shouldPost && !PostMessageW(m_window, kUpdateMessage, 0, 0)) {
        std::lock_guard<std::mutex> lock(m_contentMutex);
        m_pendingSnapshot.reset();
    }
}

void TransferProgressWindow::hide() {
    if (m_window != nullptr) {
        PostMessageW(m_window, kHideMessage, 0, 0);
    }
}

DWORD WINAPI TransferProgressWindow::uiThreadProc(LPVOID context) {
    auto* self = static_cast<TransferProgressWindow*>(context);
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
        UnregisterClassW(MAKEINTRESOURCEW(self->m_classAtom),
                         GetModuleHandleW(nullptr));
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

void TransferProgressWindow::createWindow() {
    if (m_window != nullptr) {
        return;
    }

    s_instance = this;
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&controls);

    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = &TransferProgressWindow::windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kClassName;
    m_classAtom = RegisterClassW(&windowClass);
    if (m_classAtom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Logger::write("progress window: register class failed, win32Error=" +
                      std::to_string(static_cast<uint32_t>(GetLastError())));
        return;
    }

    const LPCWSTR className = m_classAtom != 0
                                  ? MAKEINTRESOURCEW(m_classAtom)
                                  : kClassName;
    const SIZE outerSize = outerWindowSize();
    m_window = CreateWindowExW(
        kWindowExStyle,
        className, L"文件传输",
        kWindowStyle,
        CW_USEDEFAULT, CW_USEDEFAULT, outerSize.cx, outerSize.cy,
        nullptr, nullptr, instance, nullptr);
    if (m_window == nullptr) {
        Logger::write("progress window: create failed, win32Error=" +
                      std::to_string(static_cast<uint32_t>(GetLastError())));
        return;
    }

    m_fileLabel = CreateWindowExW(0, L"STATIC", L"准备传输...",
                                  WS_CHILD | WS_VISIBLE,
                                  12, 10, 340, 24, m_window,
                                  reinterpret_cast<HMENU>(
                                      static_cast<INT_PTR>(1)),
                                  instance, nullptr);
    m_progressBar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr,
                                    WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                    12, 38, 340, 16, m_window,
                                    reinterpret_cast<HMENU>(
                                        static_cast<INT_PTR>(2)),
                                    instance, nullptr);
    m_summaryLabel = CreateWindowExW(0, L"STATIC", L"",
                                     WS_CHILD | WS_VISIBLE,
                                     12, 60, 340, 24, m_window,
                                     reinterpret_cast<HMENU>(
                                    static_cast<INT_PTR>(3)),
                                    instance, nullptr);
    m_cancelButton = CreateWindowExW(
        0, L"BUTTON", L"取消",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        268, 104, 100, 30, m_window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancelButtonId)),
        instance, nullptr);
    if (m_cancelButton != nullptr) {
        const WPARAM font = static_cast<WPARAM>(
            SendMessageW(m_fileLabel, WM_GETFONT, 0, 0));
        SendMessageW(m_cancelButton, WM_SETFONT, font, TRUE);
    }
    SendMessageW(m_progressBar, PBM_SETRANGE32, 0, 10000);
    SendMessageW(m_progressBar, PBM_SETPOS, 0, 0);
}

void TransferProgressWindow::positionWindow() {
    if (m_window == nullptr) {
        return;
    }
    RECT workArea{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0)) {
        workArea.right = GetSystemMetrics(SM_CXSCREEN);
        workArea.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    const SIZE outerSize = outerWindowSize();
    const int x = workArea.right - outerSize.cx - kMargin;
    const int y = workArea.bottom - outerSize.cy - kMargin;
    SetWindowPos(m_window, HWND_TOPMOST, x, y, outerSize.cx, outerSize.cy,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void TransferProgressWindow::updateNow(const Snapshot& snapshot) {
    if (m_window == nullptr) {
        return;
    }

    std::wstring fileText = L"准备传输...";
    if (!snapshot.fileName.empty()) {
        fileText = utf8ToWide(snapshot.fileName);
        if (snapshot.fileCount > 0) {
            fileText += L" (" + std::to_wstring(snapshot.fileIndex) + L"/" +
                        std::to_wstring(snapshot.fileCount) + L")";
        }
    }
    SetWindowTextW(m_fileLabel, fileText.c_str());

    const double percent = snapshot.batchTotal > 0
        ? static_cast<double>(snapshot.batchTransferred) /
          static_cast<double>(snapshot.batchTotal) * 100.0
        : 0.0;
    const int position = static_cast<int>(percent * 100.0);
    SendMessageW(m_progressBar, PBM_SETPOS,
                 static_cast<WPARAM>(std::clamp(position, 0, 10000)), 0);

    std::wstring summary = formatBytes(snapshot.batchTransferred) + L" / " +
                           formatBytes(snapshot.batchTotal) + L" (" +
                           std::to_wstring(static_cast<uint64_t>(percent)) +
                           L"%)";
    if (snapshot.queueRemaining > 1) {
        summary += L"，剩余 " + std::to_wstring(snapshot.queueRemaining - 1) +
                   L" 个任务";
    }
    SetWindowTextW(m_summaryLabel, summary.c_str());

    m_cancelRequested.store(false);
    if (m_cancelButton != nullptr) {
        EnableWindow(m_cancelButton, TRUE);
        SetWindowTextW(m_cancelButton, L"取消");
    }
}

void TransferProgressWindow::hideNow() {
    m_visible.store(false);
    if (m_window != nullptr) {
        ShowWindow(m_window, SW_HIDE);
    }
}

LRESULT CALLBACK TransferProgressWindow::windowProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (s_instance != nullptr) {
        return s_instance->handleMessage(window, message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT TransferProgressWindow::handleMessage(HWND window, UINT message,
                                              WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case kUpdateMessage: {
            Snapshot snapshot;
            if (consumePendingSnapshot(snapshot)) {
                updateNow(snapshot);
                if (!m_visible.exchange(true)) {
                    positionWindow();
                }
            }
            return 0;
        }
        case kHideMessage:
            hideNow();
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == kCancelButtonId && HIWORD(wParam) == BN_CLICKED) {
                std::function<void()> handler;
                {
                    std::lock_guard<std::mutex> lock(m_cancelHandlerMutex);
                    handler = m_cancelHandler;
                }
                if (handler && !m_cancelRequested.exchange(true)) {
                    if (m_cancelButton != nullptr) {
                        EnableWindow(m_cancelButton, FALSE);
                        SetWindowTextW(m_cancelButton, L"正在取消...");
                    }
                    handler();
                }
            }
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_CLOSE:
            hideNow();
            return 0;
        case WM_DESTROY:
            m_window = nullptr;
            m_fileLabel = nullptr;
            m_summaryLabel = nullptr;
            m_progressBar = nullptr;
            m_cancelButton = nullptr;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool TransferProgressWindow::consumePendingSnapshot(Snapshot& snapshot) {
    std::lock_guard<std::mutex> lock(m_contentMutex);
    if (!m_pendingSnapshot) {
        return false;
    }
    snapshot = std::move(*m_pendingSnapshot);
    m_pendingSnapshot.reset();
    return true;
}

}  // namespace synccontrol
