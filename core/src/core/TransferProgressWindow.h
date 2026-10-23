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
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace synccontrol {

// A small non-activating window for transfers started by Core, including runs
// where the GUI is not present. The Win32 UI thread is separate from transfer
// and input workers so progress updates can never block clipboard delivery.
class TransferProgressWindow {
public:
    struct Snapshot {
        bool active = false;
        std::string fileName;
        uint64_t fileIndex = 0;
        uint64_t fileCount = 0;
        uint64_t fileTransferred = 0;
        uint64_t fileTotal = 0;
        uint64_t batchTransferred = 0;
        uint64_t batchTotal = 0;
        uint64_t queueRemaining = 0;
    };

    TransferProgressWindow() = default;
    ~TransferProgressWindow();

    TransferProgressWindow(const TransferProgressWindow&) = delete;
    TransferProgressWindow& operator=(const TransferProgressWindow&) = delete;

    void ensureCreated();
    void update(const Snapshot& snapshot);
    void hide();

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam,
                                       LPARAM lParam);
    static TransferProgressWindow* s_instance;
    static DWORD WINAPI uiThreadProc(LPVOID context);

    LRESULT handleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void createWindow();
    void positionWindow();
    void updateNow(const Snapshot& snapshot);
    void hideNow();
    bool consumePendingSnapshot(Snapshot& snapshot);

    HWND m_window = nullptr;
    HWND m_fileLabel = nullptr;
    HWND m_summaryLabel = nullptr;
    HWND m_progressBar = nullptr;
    ATOM m_classAtom = 0;
    std::atomic<bool> m_visible{false};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_ready{false};
    std::mutex m_contentMutex;
    std::optional<Snapshot> m_pendingSnapshot;
    std::mutex m_stateMutex;
    std::condition_variable m_stateCv;
    std::thread m_thread;
    DWORD m_threadId = 0;
};

}  // namespace synccontrol
