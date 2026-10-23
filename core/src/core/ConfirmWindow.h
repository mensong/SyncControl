#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <string>
#include <vector>

namespace synccontrol {

// A lightweight top-level confirmation for a remote clipboard file batch. Its
// UI thread stays independent from input capture, transfer, and Core work.
class ConfirmWindow {
public:
    using ResponseFn = std::function<void()>;

    ConfirmWindow() = default;
    ~ConfirmWindow();

    ConfirmWindow(const ConfirmWindow&) = delete;
    ConfirmWindow& operator=(const ConfirmWindow&) = delete;

    void setCallbacks(ResponseFn onAccept, ResponseFn onIgnore);
    void ensureCreated();
    void show(const std::string& source, size_t fileCount, uint64_t totalBytes,
              const std::vector<std::string>& previewNames);
    void hide();
    void bringToFront();
    bool isVisible() const;

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam,
                                       LPARAM lParam);
    static ConfirmWindow* s_instance;
    static DWORD WINAPI uiThreadProc(LPVOID context);

    LRESULT handleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void createWindow();
    void showNow();
    void hideNow();
    void bringToFrontNow();
    void updateContent(const std::string& source, size_t fileCount,
                       uint64_t totalBytes, const std::vector<std::string>& names);
    void respond(bool accepted);

    HWND m_window = nullptr;
    HWND m_summary = nullptr;
    HWND m_names = nullptr;
    HWND m_acceptButton = nullptr;
    HWND m_ignoreButton = nullptr;
    ATOM m_classAtom = 0;
    std::atomic<bool> m_hasTask{false};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_ready{false};
    std::mutex m_contentMutex;
    std::string m_pendingSource;
    size_t m_pendingCount = 0;
    uint64_t m_pendingBytes = 0;
    std::vector<std::string> m_pendingNames;
    std::thread m_thread;
    DWORD m_threadId = 0;
    std::mutex m_stateMutex;
    std::condition_variable m_stateCv;
    ResponseFn m_onAccept;
    ResponseFn m_onIgnore;
};

}  // namespace synccontrol
