#include "clipboard/TransferQueue.h"

#include "core/Logger.h"

namespace synccontrol {

TransferQueue::~TransferQueue() {
    stop();
}

void TransferQueue::start(ExecuteFn execute, EventFn event) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_execute = std::move(execute);
    m_event = std::move(event);
    if (!m_workerRunning) {
        m_workerRunning = true;
        m_worker = std::thread(&TransferQueue::workerLoop, this);
    }
}

void TransferQueue::stop() {
    std::vector<uint64_t> cancelled;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_workerRunning = false;
        for (Task& task : m_tasks) {
            if (task.state == State::WaitingConfirm || task.state == State::Queued ||
                task.state == State::Active) {
                task.state = State::Cancelled;
                if (task.cancel) {
                    task.cancel->store(true);
                }
                cancelled.push_back(task.id);
            }
        }
        m_workerWake.notify_all();
    }
    if (m_worker.joinable()) {
        m_worker.join();
    }
    for (const uint64_t taskId : cancelled) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (Task* task = findTaskLocked(taskId)) {
            emitEvent(*task);
        }
    }
}

void TransferQueue::setCancelFn(CancelFn cancel) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cancelFn = std::move(cancel);
}

TransferQueue::OfferResult TransferQueue::offerBatch(
    const std::vector<FileEntry>& metadata, const std::string& sourceId,
    const std::string& originPeerId, uint32_t originClipboardSequence,
    const std::string& sourceIp, uint16_t sourcePort) {
    OfferResult result;
    if (metadata.empty()) {
        return result;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_tasks.rbegin(); it != m_tasks.rend(); ++it) {
            if (!sameLogicalFiles(it->metadata, metadata)) {
                continue;
            }
            if (it->originPeerId != originPeerId ||
                it->originClipboardSequence != originClipboardSequence) {
                continue;
            }
            if (it->state == State::WaitingConfirm || it->state == State::Queued ||
                it->state == State::Active) {
                result.taskId = it->id;
                result.state = it->state;
                result.originPeerId = it->originPeerId;
                result.originClipboardSequence = it->originClipboardSequence;
                return result;
            }
            if (it->state == State::Completed && !it->result.empty()) {
                result.taskId = it->id;
                result.restored = true;
                result.state = it->state;
                result.originPeerId = it->originPeerId;
                result.originClipboardSequence = it->originClipboardSequence;
                result.files = it->result;
                return result;
            }
        }
    }

    std::vector<uint64_t> cancelled;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (Task& task : m_tasks) {
            if (task.state == State::WaitingConfirm || task.state == State::Queued) {
                task.state = State::Cancelled;
                if (task.cancel) {
                    task.cancel->store(true);
                }
                cancelled.push_back(task.id);
            } else if (task.state == State::Active) {
                if (task.cancel) {
                    task.cancel->store(true);
                }
            }
        }

        Task task;
        task.id = m_nextTaskId++;
        task.sourceId = sourceId;
        task.originPeerId = originPeerId;
        task.originClipboardSequence = originClipboardSequence;
        task.sourceIp = sourceIp;
        task.sourcePort = sourcePort;
        task.metadata = metadata;
        task.totalBytes = totalTransferBytes(metadata);
        task.state = State::WaitingConfirm;
        result.taskId = task.id;
        result.created = true;
        result.state = task.state;
        m_tasks.push_back(std::move(task));
    }

    for (const uint64_t taskId : cancelled) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (Task* task = findTaskLocked(taskId)) {
            emitEvent(*task);
        }
    }
    return result;
}

bool TransferQueue::confirm(uint64_t taskId) {
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (Task* task = findTaskLocked(taskId);
            task != nullptr && task->state == State::WaitingConfirm) {
            task->state = State::Queued;
            queued = true;
            m_workerWake.notify_one();
        }
    }
    if (queued) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (Task* task = findTaskLocked(taskId)) {
            emitEvent(*task);
        }
    }
    return queued;
}

bool TransferQueue::cancel(uint64_t taskId) {
    bool changed = false;
    Task cancelledActiveTask;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Task* task = findTaskLocked(taskId);
        if (task == nullptr) {
            return false;
        }
        if (task->state == State::WaitingConfirm || task->state == State::Queued) {
            task->state = State::Cancelled;
            changed = true;
        } else if (task->state == State::Active && task->cancel) {
            task->cancel->store(true);
            cancelledActiveTask = *task;
            m_workerWake.notify_all();
        }
    }
    if (cancelledActiveTask.id && m_cancelFn) {
        m_cancelFn(cancelledActiveTask);
    }
    if (changed) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (Task* task = findTaskLocked(taskId)) {
            emitEvent(*task);
        }
    }
    return changed;
}

bool TransferQueue::completeExternal(uint64_t taskId,
                                     std::vector<FileEntry> files) {
    bool changed = false;
    Task completed;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Task* task = findTaskLocked(taskId);
        if (task != nullptr && task->state == State::Active) {
            task->result = std::move(files);
            if (task->result.empty()) {
                task->result.clear();
                task->state = State::Failed;
            } else {
                task->state = State::Completed;
            }
            completed = *task;
            changed = true;
            m_workerWake.notify_all();
        }
    }
    if (changed) {
        emitEvent(completed);
    }
    return changed;
}

bool TransferQueue::failExternal(uint64_t taskId) {
    bool changed = false;
    Task failed;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Task* task = findTaskLocked(taskId);
        if (task != nullptr && task->state == State::Active) {
            task->result.clear();
            task->state = State::Failed;
            failed = *task;
            changed = true;
            m_workerWake.notify_all();
        }
    }
    if (changed) {
        emitEvent(failed);
    }
    return changed;
}

TransferQueue::PasteDecision TransferQueue::decidePaste(
    const std::vector<FileEntry>& metadata) const {
    PasteDecision decision;
    if (metadata.empty()) {
        return decision;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_tasks.rbegin(); it != m_tasks.rend(); ++it) {
        if (!sameLogicalFiles(it->metadata, metadata)) {
            continue;
        }
        decision.taskId = it->id;
        if (it->state == State::WaitingConfirm || it->state == State::Queued ||
            it->state == State::Active) {
            decision.action = PasteDecision::Action::Wait;
        } else if (it->state == State::Completed) {
            decision.action = PasteDecision::Action::Ready;
            decision.files = it->result;
        }
        return decision;
    }
    return decision;
}

TransferQueue::CompletedFiles TransferQueue::takeCompletedFiles(uint64_t taskId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (Task* task = findTaskLocked(taskId);
        task != nullptr && task->state == State::Completed) {
        CompletedFiles result;
        result.files = task->result;
        result.originPeerId = task->originPeerId;
        result.originClipboardSequence = task->originClipboardSequence;
        return result;
    }
    return {};
}

TransferQueue::QueueStats TransferQueue::stats() const {
    QueueStats result;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const Task& task : m_tasks) {
        if (task.state == State::Active) {
            if (result.activeTaskId == 0) {
                result.activeTaskId = task.id;
            }
            ++result.activeCount;
        } else if (task.state == State::Queued) {
            ++result.queuedCount;
        }
    }
    result.remainingCount = result.activeCount + result.queuedCount;
    return result;
}

TransferQueue::Task* TransferQueue::findTaskLocked(uint64_t taskId) {
    for (Task& task : m_tasks) {
        if (task.id == taskId) {
            return &task;
        }
    }
    return nullptr;
}

void TransferQueue::workerLoop() {
    for (;;) {
        uint64_t taskId = 0;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_workerWake.wait(lock, [this]() {
                if (!m_workerRunning) {
                    return true;
                }
                for (const Task& task : m_tasks) {
                    if (task.state == State::Queued) {
                        return true;
                    }
                }
                return false;
            });
            if (!m_workerRunning) {
                return;
            }
            for (Task& task : m_tasks) {
                if (task.state == State::Queued) {
                    task.state = State::Active;
                    task.cancel = std::make_shared<std::atomic<bool>>(false);
                    taskId = task.id;
                    break;
                }
            }
        }

        if (taskId == 0) {
            continue;
        }

        std::vector<FileEntry> files;
        bool wasCancelled = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            Task* task = findTaskLocked(taskId);
            if (task == nullptr || task->state != State::Active) {
                continue;
            }
            Logger::write("transfer queue: started task=" + std::to_string(task->id) +
                          ", source=" + task->sourceId +
                          ", count=" + std::to_string(task->metadata.size()) +
                          ", bytes=" + std::to_string(task->totalBytes));
        }
        if (m_execute) {
            std::shared_ptr<std::atomic<bool>> cancel;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (Task* task = findTaskLocked(taskId)) {
                    cancel = task->cancel;
                }
            }
            if (cancel != nullptr) {
                const Task snapshot = [this, taskId]() {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    Task* task = findTaskLocked(taskId);
                    return task != nullptr ? *task : Task{};
                }();
                ExecuteResult execution = m_execute(snapshot, *cancel);
                wasCancelled = cancel->load();
                if (execution.outcome == ExecuteResult::Outcome::ExternalPending) {
                    std::unique_lock<std::mutex> externalLock(m_mutex);
                    m_workerWake.wait(externalLock, [this, taskId]() {
                        Task* task = findTaskLocked(taskId);
                        return task == nullptr || task->state != State::Active ||
                               task->cancel->load();
                    });
                    Task* task = findTaskLocked(taskId);
                    if (task != nullptr && task->state == State::Active &&
                        task->cancel->load()) {
                        task->state = State::Cancelled;
                        task->result.clear();
                        Logger::write("transfer queue: task=" +
                                      std::to_string(task->id) + " cancelled");
                        const Task cancelled = *task;
                        externalLock.unlock();
                        emitEvent(cancelled);
                    }
                    continue;
                }
                files = std::move(execution.files);
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            Task* task = findTaskLocked(taskId);
            if (task == nullptr) {
                continue;
            }
            if (task->state != State::Active) {
                continue;
            }
            task->result = files;
            if (wasCancelled || files.empty()) {
                task->state = wasCancelled ? State::Cancelled : State::Failed;
                task->result.clear();
            } else {
                task->state = State::Completed;
            }
            Logger::write("transfer queue: task=" + std::to_string(task->id) + " " +
                          std::string([](State state) {
                              switch (state) {
                                  case State::Completed: return "completed";
                                  case State::Cancelled: return "cancelled";
                                  case State::Failed: return "failed";
                                  default: return "finished";
                              }
                          }(task->state)));
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (Task* task = findTaskLocked(taskId)) {
                emitEvent(*task);
            }
        }
    }
}

void TransferQueue::emitEvent(const Task& task) {
    if (m_event) {
        m_event(task);
    }
}

bool TransferQueue::sameLogicalFiles(const std::vector<FileEntry>& left,
                                     const std::vector<FileEntry>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t index = 0; index < left.size(); ++index) {
        if (left[index].name != right[index].name ||
            left[index].size != right[index].size ||
            left[index].isDir != right[index].isDir) {
            return false;
        }
    }
    return true;
}

uint64_t TransferQueue::totalTransferBytes(const std::vector<FileEntry>& files) {
    uint64_t total = 0;
    for (const FileEntry& entry : files) {
        if (!entry.isDir) {
            total += entry.size;
        }
    }
    return total;
}

}  // namespace synccontrol
