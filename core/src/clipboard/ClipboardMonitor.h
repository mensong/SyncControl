#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "net/Protocol.h"

namespace synccontrol {

// Tracks local clipboard content through a hidden message-only window.
// All Win32 clipboard calls must stay on the Service message-loop thread;
// the cache itself is mutex-guarded because IPC status reads cross threads.
class ClipboardMonitor {
public:
    enum class State { Empty, Text, Files };

    struct Snapshot {
        State state = State::Empty;
        std::string text;       // UTF-8, valid when state == Text
        // Local snapshots carry source paths; a pending remote snapshot has
        // empty paths until the TCP transfer completes.
        std::vector<FileEntry> files;
        uint32_t clipboardSequence = 0;
        std::string originPeerId;
        uint32_t originClipboardSequence = 0;
    };

    enum class RemotePasteDecision { Reject, Wait, Ready };

    struct RemotePasteResult {
        RemotePasteDecision decision = RemotePasteDecision::Reject;
        std::vector<FileEntry> files;
    };

    ~ClipboardMonitor();

    bool start();
    void stop();
    void clearRemoteClipboardState();

    // Re-reads the system clipboard into the cache; true when content changed.
    bool refresh();
    Snapshot snapshot() const;

    // Writes UTF-8 text to the system clipboard as CF_UNICODETEXT.
    bool setTextToClipboard(const std::string& utf8);
    // Installs delayed-render CF_HDROP data after receiving file metadata.
    bool setPendingFilesMeta(const std::vector<FileEntry>& files,
                             const std::string& originPeerId = {},
                             uint32_t originClipboardSequence = 0);
    // Writes already-downloaded files as a real CF_HDROP list.
    bool setFilesToClipboard(const std::vector<FileEntry>& files,
                             const std::string& originPeerId = {},
                             uint32_t originClipboardSequence = 0);
    // The gate is invoked during WM_RENDERFORMAT. Ready supplies local files
    // synchronously; Wait keeps the delayed-render item and shows the confirm
    // window instead of blocking Explorer on a TCP transfer.
    using PasteGateFn =
        std::function<RemotePasteResult(const std::vector<FileEntry>&)>;
    void setPasteGateHandler(PasteGateFn handler) { m_pasteGate = std::move(handler); }
    // Marshals transfer-worker completions back to the Service message loop.
    void postClipboardAction(std::function<void()> action);

    static constexpr UINT kReinstallRemoteMessage = WM_APP + 51;
    static constexpr UINT kClipboardActionMessage = WM_APP + 52;

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message,
                                       WPARAM wParam, LPARAM lParam);
    static ClipboardMonitor* s_instance;

    LRESULT handleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleRenderFormat(UINT format);
    void reinstallPendingRemoteFiles();
    bool readClipboard(Snapshot& out) const;
    bool storeIfChanged(Snapshot&& snapshot);
    void rememberClipboardUpdate();

    HWND m_window = nullptr;
    ATOM m_classAtom = 0;
    mutable std::mutex m_mutex;
    Snapshot m_cache;
    std::mutex m_pendingMutex;
    std::vector<FileEntry> m_pendingRemoteFiles;
    PasteGateFn m_pasteGate;
    std::mutex m_actionMutex;
    std::vector<std::function<void()>> m_actions;
    UINT m_filesFormat = 0;
    DWORD m_expectedClipboardSequence = 0;
    bool m_hasPendingRemoteFiles = false;
    bool m_renderingFiles = false;
};

}  // namespace synccontrol
