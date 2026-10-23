#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <json.hpp>
#include <queue>
#include <string>
#include <thread>

#include "ipc/IpcServer.h"
#include "input/InputCapture.h"
#include "clipboard/ClipboardMonitor.h"
#include "clipboard/FileTransfer.h"
#include "clipboard/TransferQueue.h"
#include "core/ConfirmWindow.h"
#include "core/TransferProgressWindow.h"
#include "layout/ScreenLayout.h"
#include "net/MasterNet.h"
#include "net/SlaveNet.h"

namespace synccontrol {

class Service {
public:
    Service();
    ~Service();

    bool startMaster(const std::string& ip, uint16_t localPort);
    bool startMaster(const nlohmann::json& layout, const std::string& bindIp,
                     uint16_t localPort);
    bool startSavedMaster();
    bool startSavedSlave();
    bool startSlave(uint16_t localPort, const std::string& masterIp,
                    uint16_t masterPort);
    void stopMode();
    void run();

private:
    nlohmann::json handleIpc(const nlohmann::json& command);
    nlohmann::json getStatusLocked();
    void processIpcCommands();
    bool startMasterWithLayout(const ScreenLayout& layout, const std::string& masterId,
                               const std::string& bindIp, uint16_t localPort);
    void applySecurityKey(const std::string& key);
    bool updateLayout(const nlohmann::json& layout);
    void handleClipboardSync(const std::string& fromPeerId, const std::string& toPeerId);
    void handleClipboardText(const std::string& fromPeerId, const std::string& utf8);
    void handleClipboardFiles(const std::string& fromPeerId,
                              const std::string& originPeerId,
                              uint32_t originClipboardSequence,
                              const std::vector<FileEntry>& files);
    void handleClipboardFilesRequest(const std::string& fromPeerId, const std::string& fromPeerIp);
    void handleRemoteFilesBatch(const std::string& sourceId,
                                const std::string& sourceIp,
                                const std::string& originPeerId,
                                uint32_t originClipboardSequence,
                                const std::vector<FileEntry>& files);
    std::vector<FileEntry> executeRemoteFileTransfer(const TransferQueue::Task& task,
                                                     const std::atomic<bool>& cancel);
    void handleTransferQueueEvent(const TransferQueue::Task& task);
    ClipboardMonitor::RemotePasteResult gateRemotePaste(
        const std::vector<FileEntry>& metadata);
    void deliverCompletedFiles(uint64_t taskId);
    void ignorePendingTransfer(uint64_t taskId);
    void handleTransferProgress(const FileTransferManager::Status& status);
    bool isLocalPeerId(const std::string& peerId);
    void reauthenticatePeers();
    std::vector<FileEntry> requestRemoteFilesFromPeer(const std::string& peerId,
                                                      const std::atomic<bool>& cancel);
    bool resolveMasterScreen(const ScreenLayout& layout, std::string& masterId) const;
    InputCapture::RouteTarget lookupRouteForCapture(const std::string& screenId,
                                                    Edge edge, float ratio);

    std::unique_ptr<MasterNet> m_master;
    std::unique_ptr<SlaveNet> m_slave;
    std::unique_ptr<IpcServer> m_ipc;
    std::unique_ptr<InputCapture> m_inputCapture;
    std::unique_ptr<ClipboardMonitor> m_clipboard;
    std::unique_ptr<FileTransferManager> m_fileTransfer;
    std::unique_ptr<TransferQueue> m_transferQueue;
    std::unique_ptr<ConfirmWindow> m_confirmWindow;
    std::unique_ptr<TransferProgressWindow> m_progressWindow;
    uint64_t m_pendingConfirmTaskId = 0;
    std::string m_pendingConfirmOrigin;
    uint32_t m_pendingConfirmSequence = 0;
    std::string m_lastIgnoredOrigin;
    uint32_t m_lastIgnoredSequence = 0;
    std::queue<nlohmann::json> m_ipcCommands;
    std::mutex m_stateMutex;
    std::mutex m_commandMutex;
    std::mutex m_layoutMutex;
    ScreenLayout m_layout;
    std::string m_masterScreenId;
    std::string m_masterIp;
    uint16_t m_masterLocalPort = 0;
    bool m_kcpOverride = false;
    KcpParameters m_kcpOverrideParameters;
    std::string m_role = "idle";
    std::string m_pendingClipboardTarget;
    std::string m_lastSentClipboardText;
    std::string m_lastSentClipboardFilesKey;
    std::string m_filesClipboardSource;
    std::thread::id m_serviceThreadId;
    bool m_running = false;
};

}  // namespace synccontrol
