#include "clipboard/FileTransfer.h"

#include <Windows.h>

#include <algorithm>
#include <string>

#include "core/Logger.h"

namespace synccontrol {
namespace {

constexpr uint32_t kSessionMagic = 0x53433631;   // 'SC61'
constexpr uint32_t kFooterMagic = 0x53433632;    // 'SC62'
constexpr uint32_t kChunkSize = 64 * 1024;
constexpr uint32_t kMaxNameLen = 32768;
constexpr uint32_t kMaxFiles = 100000;
constexpr uint64_t kMaxTotalBytes = 1ULL << 40;  // 1TB sanity cap
constexpr int kMaxRecvTimeouts = 60;             // ~5s each before giving up

enum class RecvOutcome {
    Completed,
    Cancelled,
    PeerClosed,
    Error,
    Timeout,
};

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

std::string wideToUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                          static_cast<int>(wide.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return {};
    }
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        utf8.data(), bytes, nullptr, nullptr);
    return utf8;
}

std::string systemErrorMessage(DWORD error) {
    LPWSTR buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring message(buffer != nullptr ? std::wstring(buffer, length)
                                           : std::wstring());
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n' ||
            message.back() == L'.')) {
        message.pop_back();
    }
    return wideToUtf8(message);
}

void logSystemError(const std::string& what, DWORD error) {
    const std::string text = systemErrorMessage(error);
    Logger::write("file transfer: " + what + " failed, error=" +
                  std::to_string(static_cast<uint32_t>(error)) +
                  (text.empty() ? "" : ", " + text));
}

void putU8(std::vector<uint8_t>& out, uint8_t value) {
    out.push_back(value);
}

void putU32(std::vector<uint8_t>& out, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
    }
}

void putU64(std::vector<uint8_t>& out, uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
    }
}

uint32_t getU32(const uint8_t* data, size_t offset) {
    return static_cast<uint32_t>(data[offset]) |
           (static_cast<uint32_t>(data[offset + 1]) << 8) |
           (static_cast<uint32_t>(data[offset + 2]) << 16) |
           (static_cast<uint32_t>(data[offset + 3]) << 24);
}

uint64_t getU64(const uint8_t* data, size_t offset) {
    uint64_t value = 0;
    for (int index = 7; index >= 0; --index) {
        value = (value << 8) | data[offset + static_cast<size_t>(index)];
    }
    return value;
}

void logReceiveFailure(const std::string& what, RecvOutcome outcome) {
    if (outcome == RecvOutcome::PeerClosed) {
        Logger::write("file transfer: peer closed connection while " + what);
        return;
    }
    if (outcome == RecvOutcome::Cancelled) {
        Logger::write("file transfer: receive cancelled while " + what);
        return;
    }
    logSystemError(what, WSAGetLastError());
}

// Cancel-aware exact read; socket receive timeouts only pause the loop.
RecvOutcome recvExact(TcpFileChannel& channel, uint8_t* data, size_t size,
                      const std::atomic<bool>& cancel) {
    size_t got = 0;
    int timeouts = 0;
    while (got < size) {
        if (cancel.load()) {
            return RecvOutcome::Cancelled;
        }
        const int received = channel.recvSome(data + got, size - got);
        if (received > 0) {
            got += static_cast<size_t>(received);
            timeouts = 0;
            continue;
        }
        if (received == -2 && ++timeouts < kMaxRecvTimeouts) {
            continue;
        }
        return received == 0 ? RecvOutcome::PeerClosed : RecvOutcome::Error;
    }
    return RecvOutcome::Completed;
}

// Rejects absolute paths, "..", "." and drive letters; splits into wide parts.
bool sanitizeRelativeName(const std::string& utf8Name, std::vector<std::wstring>& parts) {
    parts.clear();
    std::string normalized = utf8Name;
    for (char& c : normalized) {
        if (c == '\\') {
            c = '/';
        }
    }
    if (normalized.empty()) {
        return false;
    }
    size_t start = 0;
    for (;;) {
        const size_t slash = normalized.find('/', start);
        const size_t end = slash == std::string::npos ? normalized.size() : slash;
        const std::string part = normalized.substr(start, end - start);
        if (part.empty() || part == "." || part == ".." ||
            part.find(':') != std::string::npos) {
            return false;
        }
        const std::wstring widePart = utf8ToWide(part);
        if (widePart.empty()) {
            return false;
        }
        parts.push_back(widePart);
        if (end == normalized.size()) {
            break;
        }
        start = end + 1;
    }
    return !parts.empty();
}

std::wstring clipboardTempRoot() {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    std::wstring root(buffer.data(), length);
    if (root.empty() || root.back() != L'\\') {
        root += L'\\';
    }
    root += L"SyncControlClipboard";
    CreateDirectoryW(root.c_str(), nullptr);
    return root;
}

std::wstring makeSessionDir(const std::wstring& root) {
    if (root.empty()) {
        return {};
    }
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::wstring dir = root + L"\\t" + std::to_wstring(GetTickCount64()) +
                           L"_" + std::to_wstring(attempt);
        if (CreateDirectoryW(dir.c_str(), nullptr)) {
            return dir;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS) {
            return {};
        }
    }
    return {};
}

bool createDirectories(const std::wstring& base, const std::wstring& target,
                       std::vector<std::wstring>& created) {
    if (target.size() <= base.size()) {
        return true;
    }
    size_t pos = base.size();
    while (pos < target.size()) {
        if (target[pos] == L'\\') {
            ++pos;
            continue;
        }
        size_t next = target.find(L'\\', pos);
        if (next == std::wstring::npos) {
            next = target.size();
        }
        const std::wstring prefix = target.substr(0, next);
        if (!CreateDirectoryW(prefix.c_str(), nullptr)) {
            if (GetLastError() != ERROR_ALREADY_EXISTS) {
                return false;
            }
        } else {
            created.push_back(prefix);
        }
        pos = next;
    }
    return true;
}

void cleanupCreated(const std::vector<std::wstring>& files,
                    const std::vector<std::wstring>& dirs) {
    for (const std::wstring& file : files) {
        DeleteFileW(file.c_str());
    }
    for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) {
        RemoveDirectoryW(it->c_str());
    }
}

}  // namespace

bool sendFilesOverTcp(const std::vector<FileEntry>& files, const std::string& ip,
                      uint16_t port, const std::atomic<bool>& cancel,
                      const TransferProgressFn& progress) {
    if (cancel.load()) {
        Logger::write("file transfer: send to " + ip + ":" + std::to_string(port) +
                      " cancelled before connect");
        return false;
    }
    if (files.empty() || files.size() > kMaxFiles) {
        return false;
    }

    uint64_t totalBytes = 0;
    for (const FileEntry& entry : files) {
        if (!entry.isDir) {
            totalBytes += entry.size;
        }
    }

    TcpFileChannel channel;
    if (!channel.connect(ip, port)) {
        logSystemError("TCP connect to " + ip + ":" + std::to_string(port),
                       WSAGetLastError());
        return false;
    }
    channel.setRecvTimeoutMs(5000);
    channel.setSendTimeoutMs(10000);

    std::vector<uint8_t> header;
    putU32(header, kSessionMagic);
    putU32(header, static_cast<uint32_t>(files.size()));
    putU64(header, totalBytes);
    if (!channel.sendAll(header.data(), header.size())) {
        logSystemError("send file session header", WSAGetLastError());
        return false;
    }

    uint64_t sentTotal = 0;
    for (size_t index = 0; index < files.size(); ++index) {
        if (cancel.load()) {
            Logger::write("file transfer: send to " + ip + ":" + std::to_string(port) +
                          " cancelled before file index=" + std::to_string(index));
            return false;
        }
        const FileEntry& entry = files[index];
        std::vector<uint8_t> fileHeader;
        putU32(fileHeader, static_cast<uint32_t>(entry.name.size()));
        fileHeader.insert(fileHeader.end(), entry.name.begin(), entry.name.end());
        putU8(fileHeader, entry.isDir ? 1 : 0);
        putU64(fileHeader, entry.size);
        if (!channel.sendAll(fileHeader.data(), fileHeader.size())) {
            logSystemError("send file header for " + entry.name, WSAGetLastError());
            return false;
        }
        if (entry.isDir) {
            continue;
        }

        const std::wstring widePath = utf8ToWide(entry.path);
        const HANDLE file = CreateFileW(widePath.c_str(), GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE |
                                            FILE_SHARE_DELETE,
                                        nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            logSystemError("open source file " + entry.path, GetLastError());
            return false;
        }

        std::vector<uint8_t> buffer(kChunkSize);
        uint64_t offset = 0;
        bool ok = true;
        while (offset < entry.size) {
            if (cancel.load()) {
                Logger::write("file transfer: send to " + ip + ":" + std::to_string(port) +
                              " cancelled while reading " + entry.name);
                ok = false;
                break;
            }
            const DWORD toRead = static_cast<DWORD>(
                std::min<uint64_t>(kChunkSize, entry.size - offset));
            DWORD bytesRead = 0;
            if (!ReadFile(file, buffer.data(), toRead, &bytesRead, nullptr) ||
                bytesRead == 0) {
                logSystemError("read source file " + entry.path,
                               bytesRead == 0 ? ERROR_HANDLE_EOF : GetLastError());
                ok = false;
                break;
            }
            std::vector<uint8_t> chunkHeader;
            putU32(chunkHeader, static_cast<uint32_t>(index));
            putU64(chunkHeader, offset);
            putU32(chunkHeader, bytesRead);
            putU8(chunkHeader, offset + bytesRead >= entry.size ? 1 : 0);
            if (!channel.sendAll(chunkHeader.data(), chunkHeader.size()) ||
                !channel.sendAll(buffer.data(), bytesRead)) {
                logSystemError("send file data for " + entry.name, WSAGetLastError());
                ok = false;
                break;
            }
            offset += bytesRead;
            sentTotal += bytesRead;
            if (progress) {
                progress(entry.name, sentTotal, totalBytes);
            }
        }
        CloseHandle(file);
        if (!ok) {
            return false;
        }
    }

    std::vector<uint8_t> footer;
    putU32(footer, kFooterMagic);
    if (!channel.sendAll(footer.data(), footer.size())) {
        logSystemError("send file transfer footer", WSAGetLastError());
        return false;
    }
    return true;
}

bool receiveFilesOverTcp(TcpFileChannel& channel, const std::wstring& destRoot,
                         std::vector<FileEntry>& received, const std::atomic<bool>& cancel,
                         const TransferProgressFn& progress) {
    received.clear();
    channel.setRecvTimeoutMs(5000);

    uint8_t header[16];
    const RecvOutcome headerOutcome = recvExact(channel, header, sizeof(header), cancel);
    if (headerOutcome != RecvOutcome::Completed) {
        logReceiveFailure("receive file session header", headerOutcome);
        return false;
    }
    const uint32_t fileCount = getU32(header, 4);
    const uint64_t totalBytes = getU64(header, 8);
    if (getU32(header, 0) != kSessionMagic || fileCount > kMaxFiles ||
        totalBytes > kMaxTotalBytes) {
        Logger::write("file transfer: invalid session header, magic=0x" +
                      std::to_string(getU32(header, 0)) + ", count=" +
                      std::to_string(fileCount) + ", bytes=" +
                      std::to_string(totalBytes));
        return false;
    }

    const std::wstring root = destRoot.empty() ? clipboardTempRoot() : destRoot;
    const std::wstring sessionDir = makeSessionDir(root);
    if (sessionDir.empty()) {
        return false;
    }
    std::vector<std::wstring> createdFiles;
    std::vector<std::wstring> createdDirs{sessionDir};
    auto fail = [&]() {
        cleanupCreated(createdFiles, createdDirs);
        received.clear();
        return false;
    };

    uint64_t receivedTotal = 0;
    for (uint32_t index = 0; index < fileCount; ++index) {
        if (cancel.load()) {
            return fail();
        }
        uint8_t nameLenBytes[4];
        const RecvOutcome nameLenOutcome =
            recvExact(channel, nameLenBytes, sizeof(nameLenBytes), cancel);
        if (nameLenOutcome != RecvOutcome::Completed) {
            logReceiveFailure("receive file name length", nameLenOutcome);
            return fail();
        }
        const uint32_t nameLen = getU32(nameLenBytes, 0);
        if (nameLen == 0 || nameLen > kMaxNameLen) {
            return fail();
        }
        std::string name(nameLen, '\0');
        const RecvOutcome nameOutcome =
            recvExact(channel, reinterpret_cast<uint8_t*>(name.data()), nameLen, cancel);
        if (nameOutcome != RecvOutcome::Completed) {
            logReceiveFailure("receive file name", nameOutcome);
            return fail();
        }
        uint8_t meta[9];
        const RecvOutcome metaOutcome = recvExact(channel, meta, sizeof(meta), cancel);
        if (metaOutcome != RecvOutcome::Completed) {
            logReceiveFailure("receive file metadata for " + name, metaOutcome);
            return fail();
        }
        const bool isDir = meta[0] != 0;
        const uint64_t size = getU64(meta, 1);

        std::vector<std::wstring> parts;
        if (!sanitizeRelativeName(name, parts)) {
            return fail();
        }
        std::wstring localPath = sessionDir;
        for (const std::wstring& part : parts) {
            localPath += L'\\';
            localPath += part;
        }

        if (isDir) {
            if (!createDirectories(sessionDir, localPath, createdDirs)) {
                logSystemError("create destination directory " +
                               wideToUtf8(localPath), GetLastError());
                return fail();
            }
            FileEntry entry;
            entry.path = wideToUtf8(localPath);
            entry.name = name;
            entry.isDir = true;
            received.push_back(std::move(entry));
            continue;
        }

        const size_t parentSplit = localPath.find_last_of(L'\\');
        if (parentSplit == std::wstring::npos ||
            !createDirectories(sessionDir, localPath.substr(0, parentSplit), createdDirs)) {
            logSystemError("create destination parent " +
                           wideToUtf8(localPath.substr(0, parentSplit)),
                           GetLastError());
            return fail();
        }
        const HANDLE file = CreateFileW(localPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            logSystemError("create destination file " + wideToUtf8(localPath),
                           GetLastError());
            return fail();
        }
        createdFiles.push_back(localPath);

        uint64_t written = 0;
        bool ok = true;
        while (written < size) {
            if (cancel.load()) {
                ok = false;
                break;
            }
            uint8_t chunkHeader[17];
            const RecvOutcome chunkHeaderOutcome =
                recvExact(channel, chunkHeader, sizeof(chunkHeader), cancel);
            if (chunkHeaderOutcome != RecvOutcome::Completed) {
                logReceiveFailure("receive chunk header for " + name,
                                  chunkHeaderOutcome);
                ok = false;
                break;
            }
            const uint32_t chunkIndex = getU32(chunkHeader, 0);
            const uint64_t chunkOffset = getU64(chunkHeader, 4);
            const uint32_t chunkLen = getU32(chunkHeader, 12);
            const bool last = chunkHeader[16] != 0;
            if (chunkIndex != index || chunkOffset != written || chunkLen == 0 ||
                chunkLen > kChunkSize || written + chunkLen > size) {
                ok = false;
                break;
            }
            std::vector<uint8_t> buffer(chunkLen);
            const RecvOutcome chunkDataOutcome =
                recvExact(channel, buffer.data(), chunkLen, cancel);
            if (chunkDataOutcome != RecvOutcome::Completed) {
                logReceiveFailure("receive chunk data for " + name, chunkDataOutcome);
                ok = false;
                break;
            }
            DWORD writtenBytes = 0;
            if (!WriteFile(file, buffer.data(), chunkLen, &writtenBytes, nullptr) ||
                writtenBytes != chunkLen) {
                logSystemError("write destination file " + wideToUtf8(localPath),
                               GetLastError());
                ok = false;
                break;
            }
            written += chunkLen;
            receivedTotal += chunkLen;
            if (progress) {
                progress(name, receivedTotal, totalBytes);
            }
            if (last && written < size) {
                ok = false;
                break;
            }
        }
        CloseHandle(file);
        if (!ok || written != size) {
            return fail();
        }

        FileEntry entry;
        entry.path = wideToUtf8(localPath);
        entry.name = name;
        entry.size = size;
        entry.isDir = false;
        received.push_back(std::move(entry));
    }

    uint8_t footer[4];
    const RecvOutcome footerOutcome = recvExact(channel, footer, sizeof(footer), cancel);
    if (footerOutcome != RecvOutcome::Completed) {
        logReceiveFailure("receive file transfer footer", footerOutcome);
        return fail();
    }
    if (getU32(footer, 0) != kFooterMagic) {
        return fail();
    }
    (void)receivedTotal;
    return true;
}

FileTransferManager::~FileTransferManager() {
    stop();
}

bool FileTransferManager::start(const std::string& bindIp, uint16_t listenPort) {
    if (m_running.load()) {
        return true;
    }
    if (!m_listener.listen(bindIp, listenPort)) {
        return false;
    }
    m_running.store(true);
    m_acceptThread = std::thread(&FileTransferManager::acceptLoop, this);
    return true;
}

void FileTransferManager::stop() {
    {
        std::lock_guard<std::mutex> lock(m_cancelsMutex);
        for (const auto& cancel : m_activeCancels) {
            cancel->store(true);
        }
        m_running.store(false);
    }
    m_listener.close();  // unblocks the accept loop
    if (m_acceptThread.joinable()) {
        m_acceptThread.join();
    }
    for (std::thread& worker : m_workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    m_workers.clear();
    m_sessionBusy.store(false);
    updateStatus(false, {}, 0, 0);
}

std::shared_ptr<std::atomic<bool>> FileTransferManager::registerActiveCancel() {
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    std::lock_guard<std::mutex> lock(m_cancelsMutex);
    if (!m_running.load()) {
        return nullptr;
    }
    m_activeCancels.push_back(cancel);
    return cancel;
}

void FileTransferManager::unregisterActiveCancel(
    const std::shared_ptr<std::atomic<bool>>& cancel) {
    std::lock_guard<std::mutex> lock(m_cancelsMutex);
    m_activeCancels.erase(
        std::remove(m_activeCancels.begin(), m_activeCancels.end(), cancel),
        m_activeCancels.end());
}

void FileTransferManager::sendAsync(std::vector<FileEntry> files, const std::string& ip,
                                    uint16_t port) {
    if (files.empty() || !m_running.load()) {
        Logger::write("file transfer: cannot send, files=" +
                      std::to_string(files.size()) + ", running=" +
                      std::to_string(m_running.load() ? 1 : 0));
        return;
    }
    Logger::write("file transfer: connecting to " + ip + ":" + std::to_string(port) +
                  ", count=" + std::to_string(files.size()));
    auto cancel = registerActiveCancel();
    if (cancel == nullptr) {
        Logger::write("file transfer: cannot register send, manager is stopping");
        return;
    }
    trackWorker(std::thread([this, files = std::move(files), ip, port, cancel]() {
        const bool success =
            sendFilesOverTcp(files, ip, port, *cancel,
                             [this](const std::string& fileName, uint64_t transferred,
                                    uint64_t total) {
                                 updateStatus(true, fileName, transferred, total);
                             });
        Logger::write("file transfer: send " +
                      std::string(success ? "completed" : "failed") + " to " + ip + ":" +
                      std::to_string(port));
        updateStatus(false, {}, 0, 0);
        unregisterActiveCancel(cancel);
    }));
}

void FileTransferManager::relayAsync(const std::string& targetIp, uint16_t targetPort) {
    if (!m_running.load()) {
        return;
    }
    trackWorker(std::thread([this, targetIp, targetPort]() {
        const uint64_t token = m_completedCount.load();
        if (!waitForReceive(token, 60000)) {
            Logger::write("file transfer: relay timed out waiting for source");
            return;
        }
        ReceiveResult result = consumeReceiveResult();
        if (result.success && !result.files.empty()) {
            sendAsync(std::move(result.files), targetIp, targetPort);
        } else {
            Logger::write("file transfer: relay received no files");
        }
    }));
}

FileTransferManager::Status FileTransferManager::status() const {
    std::lock_guard<std::mutex> lock(m_statusMutex);
    return m_status;
}

uint64_t FileTransferManager::beginReceiveWait() const {
    return m_completedCount.load();
}

bool FileTransferManager::waitForReceive(uint64_t since, uint32_t timeoutMs) const {
    const uint64_t deadline = GetTickCount64() + timeoutMs;
    while (m_completedCount.load() <= since) {
        if (GetTickCount64() >= deadline || !m_running.load()) {
            return false;
        }
        Sleep(10);
    }
    return true;
}

FileTransferManager::ReceiveResult FileTransferManager::consumeReceiveResult() {
    std::lock_guard<std::mutex> lock(m_resultMutex);
    return std::move(m_lastResult);
}

void FileTransferManager::acceptLoop() {
    while (m_running.load()) {
        TcpFileChannel client;
        if (!m_listener.accept(client)) {
            if (!m_running.load()) {
                break;
            }
            Sleep(20);
            continue;
        }
        if (m_sessionBusy.exchange(true)) {
            Logger::write("file transfer: rejected connection, receive session busy");
            continue;  // one receive session at a time; extra connects drop
        }
        const SOCKET socket = client.detach();
        auto cancel = registerActiveCancel();
        if (cancel == nullptr) {
            Logger::write("file transfer: rejected connection, manager is stopping");
            m_sessionBusy.store(false);
            continue;
        }
        trackWorker(std::thread([this, socket, cancel]() {
            receiveSession(socket, *cancel);
            unregisterActiveCancel(cancel);
        }));
    }
}

void FileTransferManager::receiveSession(SOCKET socket,
                                         const std::atomic<bool>& cancel) {
    TcpFileChannel channel;
    channel.adopt(socket);
    std::vector<FileEntry> files;
    const bool success =
        receiveFilesOverTcp(channel, clipboardTempRoot(), files, cancel,
                            [this](const std::string& fileName, uint64_t transferred,
                                   uint64_t total) {
                                updateStatus(true, fileName, transferred, total);
                            });
    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        m_lastResult.success = success;
        m_lastResult.files = success ? std::move(files) : std::vector<FileEntry>{};
    }
    Logger::write(std::string("file transfer: receive ") +
                  (success ? "completed" : "failed"));
    if (!success) {
        updateStatus(false, {}, 0, 0);
    }
    m_completedCount.fetch_add(1);
    m_sessionBusy.store(false);
}

void FileTransferManager::updateStatus(bool active, const std::string& fileName,
                                       uint64_t transferred, uint64_t total) {
    std::lock_guard<std::mutex> lock(m_statusMutex);
    m_status.active = active;
    m_status.fileName = fileName;
    m_status.transferred = transferred;
    m_status.total = total;
}

void FileTransferManager::trackWorker(std::thread worker) {
    m_workers.push_back(std::move(worker));
}

}  // namespace synccontrol
