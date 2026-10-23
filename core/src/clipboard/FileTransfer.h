#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net/Protocol.h"
#include "net/TcpFileChannel.h"

namespace synccontrol {

using TransferProgressFn =
    std::function<void(const struct TransferProgress& progress)>;

struct TransferProgress {
    std::string fileName;
    uint64_t fileIndex = 0;
    uint64_t fileCount = 0;
    uint64_t fileTransferred = 0;
    uint64_t fileTotal = 0;
        uint64_t batchTransferred = 0;
        uint64_t batchTotal = 0;
        uint64_t chunkBytes = 0;
};

// Wire-level session: sender connects, streams a session header, per-file
// headers, and 64KB data chunks, then a footer. Both helpers block and must
// run on worker threads; cancel aborts at chunk granularity.
bool sendFilesOverTcp(const std::vector<FileEntry>& files, const std::string& ip,
                      uint16_t port, const std::atomic<bool>& cancel,
                      const TransferProgressFn& progress);
bool receiveFilesOverTcp(TcpFileChannel& channel, const std::wstring& destRoot,
                         std::vector<FileEntry>& received, const std::atomic<bool>& cancel,
                         const TransferProgressFn& progress);

// Owns the TCP listener plus every transfer worker thread. One receive
// session at a time; senders are independent. Used by both master and slave
// roles so either side can paste files.
class FileTransferManager {
public:
    struct Status {
        bool active = false;
        std::string fileName;
        uint64_t fileIndex = 0;
        uint64_t fileCount = 0;
        uint64_t fileTransferred = 0;
        uint64_t fileTotal = 0;
        uint64_t transferred = 0;
        uint64_t total = 0;
    };

    FileTransferManager() = default;
    ~FileTransferManager();

    FileTransferManager(const FileTransferManager&) = delete;
    FileTransferManager& operator=(const FileTransferManager&) = delete;

    bool start(const std::string& bindIp, uint16_t listenPort);
    void stop();

    void sendAsync(std::vector<FileEntry> files, const std::string& ip, uint16_t port);
    // Worker: waits for the next receive completion, then forwards the
    // received files to targetIp (master-side relay between two peers).
    void relayAsync(const std::string& targetIp, uint16_t targetPort);

    Status status() const;
    void setProgressHandler(std::function<void(const Status&)> handler);
    void setRemoteInputActiveHandler(std::function<bool()> handler);

    // Render waits: capture the completion counter, then wait for it to grow.
    uint64_t beginReceiveWait() const;
    bool waitForReceive(uint64_t since, uint32_t timeoutMs) const;

    struct ReceiveResult {
        bool success = false;
        std::vector<FileEntry> files;
    };
    ReceiveResult consumeReceiveResult();

private:
    void acceptLoop();
    void receiveSession(SOCKET socket, const std::atomic<bool>& cancel);
    std::shared_ptr<std::atomic<bool>> registerActiveCancel();
    void unregisterActiveCancel(const std::shared_ptr<std::atomic<bool>>& cancel);
    void updateStatus(bool active, const std::string& fileName, uint64_t transferred,
                      uint64_t total);
    void updateStatus(bool active, const TransferProgress& progress);
    void throttleForRemoteInput(uint64_t bytes);
    void trackWorker(std::thread worker);

    TcpFileChannel m_listener;
    std::thread m_acceptThread;
    std::vector<std::thread> m_workers;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_sessionBusy{false};

    std::mutex m_cancelsMutex;
    std::vector<std::shared_ptr<std::atomic<bool>>> m_activeCancels;
    mutable std::mutex m_statusMutex;
    Status m_status;
    std::function<void(const Status&)> m_progressHandler;
    mutable std::mutex m_progressHandlerMutex;
    std::function<bool()> m_remoteInputActiveHandler;
    mutable std::mutex m_remoteInputActiveHandlerMutex;
    mutable std::mutex m_throttleMutex;
    uint64_t m_throttleWindowStart = 0;
    uint64_t m_throttleWindowBytes = 0;
    std::atomic<uint64_t> m_completedCount{0};
    mutable std::mutex m_resultMutex;
    ReceiveResult m_lastResult;
};

}  // namespace synccontrol
