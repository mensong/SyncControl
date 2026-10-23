#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net/Protocol.h"

namespace synccontrol {

// Serializes confirmed remote-file downloads. A task is presented to the user
// before any network traffic starts; the worker performs one TCP session at a
// time and can abort an active transfer at chunk boundaries.
class TransferQueue {
public:
    enum class State {
        WaitingConfirm,
        Queued,
        Active,
        Completed,
        Failed,
        Cancelled,
    };

    struct Task {
        uint64_t id = 0;
        std::string sourceId;
        std::string originPeerId;
        uint32_t originClipboardSequence = 0;
        std::string sourceIp;
        uint16_t sourcePort = 0;
        std::vector<FileEntry> metadata;
        uint64_t totalBytes = 0;
        State state = State::WaitingConfirm;
        std::vector<FileEntry> result;
        std::shared_ptr<std::atomic<bool>> cancel;
    };

    struct OfferResult {
        uint64_t taskId = 0;
        bool created = false;
        bool restored = false;
        State state = State::Cancelled;
        std::string originPeerId;
        uint32_t originClipboardSequence = 0;
        std::vector<FileEntry> files;
    };

    struct CompletedFiles {
        std::vector<FileEntry> files;
        std::string originPeerId;
        uint32_t originClipboardSequence = 0;
    };

    struct QueueStats {
        size_t activeCount = 0;
        size_t queuedCount = 0;
        size_t remainingCount = 0;
    };

    struct PasteDecision {
        enum class Action { Reject, Wait, Ready };

        Action action = Action::Reject;
        uint64_t taskId = 0;
        std::vector<FileEntry> files;
    };

    using ExecuteFn = std::function<std::vector<FileEntry>(
        const Task&, const std::atomic<bool>&)>;
    using EventFn = std::function<void(const Task&)>;

    TransferQueue() = default;
    ~TransferQueue();

    TransferQueue(const TransferQueue&) = delete;
    TransferQueue& operator=(const TransferQueue&) = delete;

    void start(ExecuteFn execute, EventFn event);
    void stop();

    OfferResult offerBatch(const std::vector<FileEntry>& metadata,
                           const std::string& sourceId,
                           const std::string& originPeerId,
                           uint32_t originClipboardSequence,
                           const std::string& sourceIp,
                           uint16_t sourcePort);
    bool confirm(uint64_t taskId);
    bool cancel(uint64_t taskId);
    PasteDecision decidePaste(const std::vector<FileEntry>& metadata) const;
    CompletedFiles takeCompletedFiles(uint64_t taskId);
    QueueStats stats() const;

private:
    Task* findTaskLocked(uint64_t taskId);
    void workerLoop();
    void emitEvent(const Task& task);
    static bool sameLogicalFiles(const std::vector<FileEntry>& left,
                                 const std::vector<FileEntry>& right);
    static uint64_t totalTransferBytes(const std::vector<FileEntry>& files);

    ExecuteFn m_execute;
    EventFn m_event;
    std::deque<Task> m_tasks;
    mutable std::mutex m_mutex;
    std::condition_variable m_workerWake;
    std::thread m_worker;
    bool m_workerRunning = false;
    uint64_t m_nextTaskId = 1;
};

}  // namespace synccontrol
