#include "core/Service.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>

#include "core/Logger.h"
#include "core/Security.h"

namespace synccontrol {
namespace {

std::string localHostnameLower() {
    char buffer[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = static_cast<DWORD>(sizeof(buffer));
    if (!GetComputerNameA(buffer, &size)) {
        return {};
    }
    std::string name(buffer, size);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return name;
}

bool iequals(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t index = 0; index < left.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

bool parseKcpParameters(const nlohmann::json& value, KcpParameters& parameters) {
    if (!value.is_object()) {
        return false;
    }
    const int nodelay = value.value("nodelay", 1);
    const int interval = value.value("interval", 10);
    const int resend = value.value("resend", 2);
    const int nc = value.value("nc", 1);
    const int sendWindow = value.value("sendWindow", 128);
    const int receiveWindow = value.value("receiveWindow", 128);
    if ((nodelay != 0 && nodelay != 1) || interval < 1 || interval > 1000 ||
        resend < 0 || resend > 5 || (nc != 0 && nc != 1) ||
        sendWindow < 1 || sendWindow > 1024 || receiveWindow < 1 ||
        receiveWindow > 1024) {
        return false;
    }
    parameters = {nodelay, interval, resend, nc, sendWindow, receiveWindow};
    return true;
}

nlohmann::json kcpParametersToJson(const KcpParameters& parameters) {
    return {
        {"nodelay", parameters.nodelay},
        {"interval", parameters.interval},
        {"resend", parameters.resend},
        {"nc", parameters.nc},
        {"sendWindow", parameters.sendWindow},
        {"receiveWindow", parameters.receiveWindow},
    };
}

size_t topLevelCount(const std::vector<FileEntry>& files) {
    size_t count = 0;
    for (const FileEntry& entry : files) {
        if (entry.name.find('/') == std::string::npos) {
            ++count;
        }
    }
    return count;
}

std::vector<std::string> topLevelPreview(const std::vector<FileEntry>& files) {
    std::vector<std::string> names;
    for (const FileEntry& entry : files) {
        if (entry.name.find('/') == std::string::npos) {
            names.push_back(entry.name);
            if (names.size() >= 8) {
                break;
            }
        }
    }
    return names;
}

// Legacy single-IP mode keeps the P2 behavior by materializing an implicit
// full-edge routing table and running it through the normal edge router.
nlohmann::json buildSinglePeerLayout(const std::string& ip) {
    const auto fullEdge = [](const std::string& toId, const std::string& toEdge) {
        return nlohmann::json::array({nlohmann::json{
            {"rangeStart", 0.0},
            {"rangeEnd", 1.0},
            {"toId", toId},
            {"toEdge", toEdge},
        }});
    };

    nlohmann::json layout;
    layout["screens"] = nlohmann::json::array({
        nlohmann::json{
            {"id", "master"},
            {"hostname", ""},
            {"ip", ""},
            {"screenW", GetSystemMetrics(SM_CXSCREEN)},
            {"screenH", GetSystemMetrics(SM_CYSCREEN)},
        },
        nlohmann::json{
            {"id", ip},
            {"hostname", ""},
            {"ip", ip},
            {"screenW", 0},
            {"screenH", 0},
        },
    });

    nlohmann::json& masterRouting = layout["routing"]["master"];
    masterRouting["top"] = fullEdge(ip, "bottom");
    masterRouting["bottom"] = fullEdge(ip, "top");
    masterRouting["left"] = fullEdge(ip, "right");
    masterRouting["right"] = fullEdge(ip, "left");

    nlohmann::json& peerRouting = layout["routing"][ip];
    peerRouting["top"] = fullEdge("master", "bottom");
    peerRouting["bottom"] = fullEdge("master", "top");
    peerRouting["left"] = fullEdge("master", "right");
    peerRouting["right"] = fullEdge("master", "left");
    return layout;
}

std::string savedMasterLayoutPath() {
    char buffer[MAX_PATH] = {};
    const UINT size = GetEnvironmentVariableA("LOCALAPPDATA", buffer, MAX_PATH);
    if (size == 0 || size >= MAX_PATH) {
        return {};
    }
    const std::string directory = std::string(buffer, size) + "\\SyncControl";
    CreateDirectoryA(directory.c_str(), nullptr);
    return directory + "\\master-layout.json";
}

void persistMasterLayout(const ScreenLayout& layout) {
    const std::string path = savedMasterLayoutPath();
    if (path.empty()) {
        Logger::write("master: cannot resolve saved layout path");
        return;
    }
    if (!layout.saveToFile(path)) {
        Logger::write("master: failed to save layout for restore");
    }
}

struct ServiceSettings {
    std::string role = "master";
    std::string bindIp = "0.0.0.0";
    uint16_t port = 19881;
    std::string masterIp;
    uint16_t masterPort = 19881;
    uint16_t slavePort = 19881;
    std::string securityKey;
};

std::string serviceConfigPath() {
    char buffer[MAX_PATH] = {};
    const UINT size = GetEnvironmentVariableA("LOCALAPPDATA", buffer, MAX_PATH);
    if (size == 0 || size >= MAX_PATH) {
        return {};
    }
    return std::string(buffer, size) + "\\SyncControl\\service-config.json";
}

ServiceSettings loadServiceSettings() {
    ServiceSettings settings;
    const std::string path = serviceConfigPath();
    if (path.empty()) {
        return settings;
    }
    std::ifstream file(path);
    if (!file) {
        return settings;
    }
    try {
        nlohmann::json value;
        file >> value;
        settings.role = value.value("role", settings.role);
        settings.bindIp = value.value("bindIp", settings.bindIp);
        settings.port = static_cast<uint16_t>(value.value("port", 19881));
        settings.masterIp = value.value("masterIp", std::string());
        settings.masterPort = static_cast<uint16_t>(value.value("masterPort", 19881));
        settings.slavePort = static_cast<uint16_t>(value.value("slavePort", 19881));
        settings.securityKey = value.value("securityKey", std::string());
    } catch (const std::exception&) {
        Logger::write("service: invalid service-config.json, using defaults");
    }
    return settings;
}

std::string nextClipboardImageTransferId() {
    static uint64_t counter = 0;
    return "img-" + std::to_string(GetTickCount64()) + "-" +
           std::to_string(++counter);
}

bool saveClipboardImageFile(const std::string& transferId,
                            const std::vector<uint8_t>& png,
                            std::string& path) {
    char root[MAX_PATH]{};
    const UINT length = GetEnvironmentVariableA("LOCALAPPDATA", root, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return false;
    }
    const std::string base = std::string(root, length) + "\\SyncControl";
    if (!CreateDirectoryA(base.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    if (!CreateDirectoryA((base + "\\clipboard").c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    const std::string images = base + "\\clipboard\\images";
    if (!CreateDirectoryA(images.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    path = images + "\\" + transferId + ".png";
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(reinterpret_cast<const char*>(png.data()),
               static_cast<std::streamsize>(png.size()));
    return static_cast<bool>(file);
}

std::vector<uint8_t> readFileBytes(const std::string& path,
                                   uint64_t maxBytes) {
    if (path.empty()) {
        return {};
    }
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }
    const std::streamoff end = file.tellg();
    if (end < 0 || static_cast<uint64_t>(end) > maxBytes) {
        return {};
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(end));
    file.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        file.read(reinterpret_cast<char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    return file ? bytes : std::vector<uint8_t>{};
}

bool isImageTransferFile(const FileEntry& entry) {
    return !entry.isDir && entry.name.size() > 8 &&
           entry.name.compare(0, 4, "img-") == 0 &&
           entry.name.compare(entry.name.size() - 4, 4, ".png") == 0;
}

std::string nextClipboardRichTransferId() {
    static uint64_t counter = 0;
    return "rich-" + std::to_string(GetTickCount64()) + "-" +
           std::to_string(++counter);
}

std::string richExtension(ClipboardRichKind kind) {
    return kind == ClipboardRichKind::Rtf ? ".rtf" : ".html";
}

bool isRichTransferFile(const FileEntry& entry) {
    if (entry.isDir || entry.name.size() < 14 ||
        entry.name.compare(0, 5, "rich-") != 0) {
        return false;
    }
    return entry.name.compare(entry.name.size() - 5, 5, ".html") == 0 ||
           entry.name.compare(entry.name.size() - 4, 4, ".rtf") == 0;
}

bool saveClipboardRichFile(const std::string& transferId,
                           ClipboardRichKind kind,
                           const std::vector<uint8_t>& richData,
                           const std::string& companionTextUtf8,
                           std::string& richPath,
                           std::string& textPath) {
    char root[MAX_PATH]{};
    const UINT length = GetEnvironmentVariableA("LOCALAPPDATA", root, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return false;
    }
    const std::string base = std::string(root, length) + "\\SyncControl";
    if (!CreateDirectoryA(base.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    if (!CreateDirectoryA((base + "\\clipboard").c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    const std::string rich = base + "\\clipboard\\rich";
    if (!CreateDirectoryA(rich.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    richPath = rich + "\\" + transferId + richExtension(kind);
    textPath = rich + "\\" + transferId + ".txt";
    std::ofstream richFile(richPath, std::ios::binary | std::ios::trunc);
    if (!richFile) {
        return false;
    }
    richFile.write(reinterpret_cast<const char*>(richData.data()),
                   static_cast<std::streamsize>(richData.size()));
    richFile.close();
    if (!richFile) {
        return false;
    }
    const std::vector<uint8_t> textData(companionTextUtf8.begin(),
                                        companionTextUtf8.end());
    std::ofstream textFile(textPath, std::ios::binary | std::ios::trunc);
    if (!textFile) {
        return false;
    }
    textFile.write(reinterpret_cast<const char*>(textData.data()),
                   static_cast<std::streamsize>(textData.size()));
    return static_cast<bool>(textFile);
}

}  // namespace

Service::Service() {
    FileTransferManager::cleanupTempSessions();
    m_ipc = std::make_unique<IpcServer>([this](const nlohmann::json& command) {
        return handleIpc(command);
    });
    m_ipc->start();
    m_transferQueue = std::make_unique<TransferQueue>();
    m_confirmWindow = std::make_unique<ConfirmWindow>();
    m_confirmWindow->setCallbacks(
        [this]() {
            const uint64_t taskId = m_pendingConfirmTaskId;
            m_pendingConfirmTaskId = 0;
            if (m_transferQueue && taskId != 0) {
                m_transferQueue->confirm(taskId);
            }
        },
        [this]() {
            const uint64_t taskId = m_pendingConfirmTaskId;
            ignorePendingTransfer(taskId);
        });
    m_confirmWindow->ensureCreated();
    m_progressWindow = std::make_unique<TransferProgressWindow>();
    m_progressWindow->ensureCreated();
    m_transferQueue->setCancelFn([this](const TransferQueue::Task& task) {
        cancelExternalFileTransfer(task);
    });
    m_transferQueue->start(
        [this](const TransferQueue::Task& task, const std::atomic<bool>& cancel) {
            return executeRemoteFileTransfer(task, cancel);
        },
        [this](const TransferQueue::Task& task) {
            handleTransferQueueEvent(task);
        });
}

Service::~Service() {
    stopMode();
    if (m_fileTransfer) {
        m_fileTransfer->setProgressHandler(nullptr);
    }
    if (m_ipc) {
        m_ipc->stop();
    }
}

bool Service::startMaster(const std::string& ip, uint16_t localPort) {
    if (ip.empty()) {
        return false;
    }
    ScreenLayout layout;
    if (!layout.loadFromJson(buildSinglePeerLayout(ip))) {
        return false;
    }
    std::string masterId;
    if (!resolveMasterScreen(layout, masterId)) {
        return false;
    }
    return startMasterWithLayout(layout, masterId, "0.0.0.0", localPort);
}

bool Service::startMaster(const nlohmann::json& layoutJson, const std::string& bindIp,
                          uint16_t localPort) {
    ScreenLayout layout;
    if (!layout.loadFromJson(layoutJson)) {
        std::cerr << "start_master: invalid layout\n";
        Logger::write("start_master: invalid layout rejected");
        return false;
    }
    std::string masterId;
    if (!resolveMasterScreen(layout, masterId)) {
        std::cerr << "start_master: cannot identify master screen\n";
        Logger::write("start_master: cannot identify master screen");
        return false;
    }
    return startMasterWithLayout(layout, masterId, bindIp, localPort);
}

bool Service::startSavedMaster() {
    const ServiceSettings settings = loadServiceSettings();
    applySecurityKey(settings.securityKey);
    ScreenLayout layout;
    if (!layout.loadFromFile(savedMasterLayoutPath())) {
        Logger::write("master: no valid saved layout to restore");
        return false;
    }
    std::string masterId;
    if (!resolveMasterScreen(layout, masterId)) {
        Logger::write("master: saved layout has no local master screen");
        return false;
    }
    return startMasterWithLayout(layout, masterId, settings.bindIp, settings.port);
}

bool Service::startSavedSlave() {
    const ServiceSettings settings = loadServiceSettings();
    applySecurityKey(settings.securityKey);
    return startSlave(settings.slavePort, settings.masterIp, settings.masterPort);
}

bool Service::startSavedService(const std::string& requestedRole) {
    const ServiceSettings settings = loadServiceSettings();
    const std::string& role = requestedRole.empty() ? settings.role : requestedRole;
    if (role == "master") {
        return startSavedMaster();
    }
    if (role == "slave") {
        return startSavedSlave();
    }
    Logger::write("service: unknown configured role " + role);
    return false;
}

bool Service::startMasterWithLayout(const ScreenLayout& layout, const std::string& masterId,
                                    const std::string& bindIp, uint16_t localPort) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    stopMode();

    std::vector<MasterNet::PeerInit> peerInits;
    for (const ScreenNode& node : layout.nodes()) {
        if (node.id == masterId || node.ip.empty()) {
            continue;
        }
        peerInits.push_back({node.id, node.ip,
                             static_cast<uint16_t>(node.port > 0 ? node.port : 19881)});
    }

    if (!m_master) {
        m_master = std::make_unique<MasterNet>();
    }
    m_master->setPeerId(masterId);
    if (!m_master->start(bindIp, peerInits, localPort != 0 ? localPort : 19881)) {
        m_role = "idle";
        return false;
    }
    if (m_kcpOverride) {
        m_master->setKcpOverride(m_kcpOverrideParameters);
    }
    if (!m_fileTransfer) {
        m_fileTransfer = std::make_unique<FileTransferManager>();
    }
    m_fileTransfer->setProgressHandler([this](const FileTransferManager::Status& status) {
        handleTransferProgress(status);
    });
    m_fileTransfer->setReceiveCompletedHandler([this](bool success,
                                                      std::vector<FileEntry> files) {
        handleReceiveCompleted(success, std::move(files));
    });
    m_fileTransfer->setRemoteInputActiveHandler([this]() {
        if (m_role == "master") {
            return m_master != nullptr && !m_master->focusedPeerId().empty();
        }
        if (m_role == "slave") {
            return m_slave != nullptr && m_slave->isInputActive();
        }
        return false;
    });
    if (!m_fileTransfer->start(bindIp.empty() ? "0.0.0.0" : bindIp,
                               localPort != 0 ? localPort : 19881)) {
        m_master->stop();
        m_role = "idle";
        std::cerr << "start_master: file transfer listener failed\n";
        return false;
    }
    if (m_transferQueue) {
        m_transferQueue->start(
            [this](const TransferQueue::Task& task, const std::atomic<bool>& cancel) {
                return executeRemoteFileTransfer(task, cancel);
            },
            [this](const TransferQueue::Task& task) {
                handleTransferQueueEvent(task);
            });
    }

    {
        std::lock_guard<std::mutex> layoutLock(m_layoutMutex);
        m_layout = layout;
        m_masterScreenId = masterId;
    }
    m_role = "master";
    m_masterLocalPort = localPort;

    if (!m_inputCapture) {
        m_inputCapture = std::make_unique<InputCapture>();
    }
    InputCapture::Callbacks callbacks{};
    callbacks.sendMouseMove = [this](uint16_t x, uint16_t y) {
        if (m_master) {
            m_master->queueMouseMove(x, y);
        }
    };
    callbacks.sendMouseButton = [this](MouseButtonCode button, MouseAction action) {
        if (m_master) {
            m_master->sendMouseButton(button, action);
        }
    };
    callbacks.sendMouseWheel = [this](WheelDirection direction, int16_t delta) {
        if (m_master) {
            m_master->sendMouseWheel(direction, delta);
        }
    };
    callbacks.sendKeyEvent = [this](bool down, uint16_t vkCode, uint16_t scanCode,
                                    bool extendedKey) {
        if (m_master) {
            m_master->sendKeyEvent(down, vkCode, scanCode, extendedKey);
        }
    };
    callbacks.localScreenId = [this]() {
        std::lock_guard<std::mutex> layoutLock(m_layoutMutex);
        return m_masterScreenId;
    };
    callbacks.lookupRoute = [this](const std::string& screenId, Edge edge, float ratio) {
        return lookupRouteForCapture(screenId, edge, ratio);
    };
    callbacks.sendSwitchIn = [this](const std::string& peerId, uint16_t x, uint16_t y) {
        return m_master != nullptr && m_master->sendSwitchInTo(peerId, x, y);
    };
    callbacks.sendSwitchOut = [this](const std::string& peerId) {
        return m_master != nullptr && m_master->sendSwitchOutFrom(peerId);
    };
    callbacks.clipboardSync = [this](const std::string& fromPeerId,
                                     const std::string& toPeerId) {
        handleClipboardSync(fromPeerId, toPeerId);
    };
    m_master->onClipboardText = [this](const std::string& fromPeerId,
                                       const std::string& utf8) {
        handleClipboardText(fromPeerId, utf8);
    };
    m_master->onClipboardImageMeta = [this](const std::string& fromPeerId,
                                            const ClipboardImageMeta& meta) {
        handleClipboardImageMeta(fromPeerId, meta);
    };
    m_master->onClipboardRichMeta = [this](const std::string& fromPeerId,
                                           const ClipboardRichMeta& meta) {
        handleClipboardRichMeta(fromPeerId, meta);
    };
    m_master->onClipboardFiles = [this](const std::string& fromPeerId,
                                        const std::string& originPeerId,
                                        uint32_t originClipboardSequence,
                                        const std::vector<FileEntry>& files) {
        handleClipboardFiles(fromPeerId, originPeerId, originClipboardSequence, files);
    };
    m_master->onClipboardFilesRequest = [this](const std::string& fromPeerId,
                                               const std::string& fromPeerIp) {
        handleClipboardFilesRequest(fromPeerId, fromPeerIp);
    };
    m_master->onClipboardImageRequest = [this](const std::string& fromPeerId,
                                               const std::string& fromPeerIp,
                                               const std::string& transferId) {
        handleClipboardImageRequest(fromPeerId, fromPeerIp, transferId);
    };
    m_master->onClipboardRichRequest = [this](const std::string& fromPeerId,
                                              const std::string& fromPeerIp,
                                              const std::string& transferId) {
        handleClipboardRichRequest(fromPeerId, fromPeerIp, transferId);
    };
    if (!m_inputCapture->start(callbacks)) {
        m_master->stop();
        m_fileTransfer->stop();
        m_role = "idle";
        return false;
    }

    if (!m_clipboard) {
        m_clipboard = std::make_unique<ClipboardMonitor>();
    }
    if (!m_clipboard->start()) {
        std::cerr << "start_master: clipboard monitor failed\n";
    }
    m_clipboard->setPasteGateHandler([this](const std::vector<FileEntry>& metadata) {
        return gateRemotePaste(metadata);
    });
    Logger::write("master: started with " + std::to_string(peerInits.size()) + " peers");
    persistMasterLayout(layout);
    return true;
}

bool Service::startSlave(uint16_t localPort, const std::string& masterIp,
                         uint16_t masterPort) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    stopMode();
    if (!m_clipboard) {
        m_clipboard = std::make_unique<ClipboardMonitor>();
    }
    if (!m_clipboard->start()) {
        std::cerr << "start_slave: clipboard monitor failed\n";
    }
    m_clipboard->setPasteGateHandler([this](const std::vector<FileEntry>& metadata) {
        return gateRemotePaste(metadata);
    });
    if (!m_slave) {
        m_slave = std::make_unique<SlaveNet>();
    }
    m_slave->setRemoteFilesMetaHandler([this](const std::string& originPeerId,
                                              uint32_t originClipboardSequence,
                                              const std::vector<FileEntry>& files) {
        std::string sourceIp = "master";
        if (m_slave && !m_slave->peers().empty()) {
            sourceIp = endpointAddressToString(m_slave->peers().front().endpoint);
        }
        handleRemoteFilesBatch("master", sourceIp, originPeerId,
                               originClipboardSequence, files);
    });
    m_slave->setRemoteImageMetaHandler([this](const ClipboardImageMeta& meta) {
        std::string sourceIp = "master";
        if (m_slave && !m_slave->peers().empty()) {
            sourceIp = endpointAddressToString(m_slave->peers().front().endpoint);
        }
        handleRemoteImageBatch("master", sourceIp, meta);
    });
    m_slave->setRemoteRichMetaHandler([this](const ClipboardRichMeta& meta) {
        std::string sourceIp = "master";
        if (m_slave && !m_slave->peers().empty()) {
            sourceIp = endpointAddressToString(m_slave->peers().front().endpoint);
        }
        handleRemoteRichBatch("master", sourceIp, meta);
    });
    m_slave->setClipboardMonitor(m_clipboard.get());
    if (!m_fileTransfer) {
        m_fileTransfer = std::make_unique<FileTransferManager>();
    }
    m_fileTransfer->setProgressHandler([this](const FileTransferManager::Status& status) {
        handleTransferProgress(status);
    });
    m_fileTransfer->setReceiveCompletedHandler([this](bool success,
                                                      std::vector<FileEntry> files) {
        handleReceiveCompleted(success, std::move(files));
    });
    m_fileTransfer->setRemoteInputActiveHandler([this]() {
        if (m_role == "master") {
            return m_master != nullptr && !m_master->focusedPeerId().empty();
        }
        if (m_role == "slave") {
            return m_slave != nullptr && m_slave->isInputActive();
        }
        return false;
    });
    if (!m_fileTransfer->start("0.0.0.0", localPort != 0 ? localPort : 19881)) {
        m_role = "idle";
        std::cerr << "start_slave: file transfer listener failed\n";
        return false;
    }
    m_slave->setFileTransfer(m_fileTransfer.get());
    if (m_transferQueue) {
        m_transferQueue->start(
            [this](const TransferQueue::Task& task, const std::atomic<bool>& cancel) {
                return executeRemoteFileTransfer(task, cancel);
            },
            [this](const TransferQueue::Task& task) {
                handleTransferQueueEvent(task);
            });
    }
    if (m_kcpOverride) {
        m_slave->setKcpOverride(m_kcpOverrideParameters);
    }
    const bool started = m_slave->start(localPort != 0 ? localPort : 19881,
                                        masterIp, masterPort != 0 ? masterPort : 19881);
    if (started) {
        m_role = "slave";
        Logger::write("slave: started on port " + std::to_string(localPort) +
                      ", master " + masterIp + ":" + std::to_string(masterPort));
    } else {
        m_fileTransfer->stop();
        if (m_transferQueue) {
            m_transferQueue->stop();
        }
    }
    return started;
}

void Service::applySecurityKey(const std::string& key) {
    const bool ok = key.empty() ? SecurityKeyStore::clearKey()
                                : SecurityKeyStore::importKey(key);
    if (!ok) {
        Logger::write("service: failed to apply security key settings");
    }
}

void Service::stopMode() {
    Logger::write("service: mode stopped");
    if (m_fileTransfer) {
        m_fileTransfer->setReceiveCompletedHandler(nullptr);
    }
    if (m_transferQueue) {
        m_transferQueue->stop();
    }
    m_pendingConfirmTaskId = 0;
    if (m_confirmWindow) {
        m_confirmWindow->hide();
    }
    if (m_progressWindow) {
        m_progressWindow->hide();
    }
    if (m_inputCapture) {
        m_inputCapture->stop();
    }
    if (m_master) {
        m_master->stop();
    }
    if (m_slave) {
        m_slave->stop();
    }
    if (m_fileTransfer) {
        m_fileTransfer->stop();
        m_fileTransfer->setProgressHandler(nullptr);
    }
    if (m_clipboard) {
        m_clipboard->clearRemoteClipboardState();
        m_clipboard->setPasteGateHandler(nullptr);
    }
    m_pendingClipboardTarget.clear();
    m_lastSentClipboardText.clear();
    m_lastSentClipboardImageKey.clear();
    m_lastSentClipboardRichKey.clear();
    m_lastSentClipboardFilesKey.clear();
    m_filesClipboardSource.clear();
    m_imageClipboardSource.clear();
    m_richClipboardSource.clear();
    m_clipboardImages.clear();
    m_clipboardRich.clear();
    m_pendingConfirmTaskId = 0;
    m_pendingConfirmOrigin.clear();
    m_pendingConfirmSequence = 0;
    m_lastIgnoredOrigin.clear();
    m_lastIgnoredSequence = 0;
    {
        std::lock_guard<std::mutex> lock(m_activeFileMutex);
        m_activeFileTaskId = 0;
    }
    m_role = "idle";
}

// 钩子回调线程（Service 消息循环）在 SWITCH_IN 前调用；源端为 Master 时直接
// 推送本地文本，源端为被控端时先发请求，等回包后再转发给目标端。
void Service::handleClipboardSync(const std::string& fromPeerId,
                                  const std::string& toPeerId) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (m_master == nullptr || m_clipboard == nullptr) {
        return;
    }

    if (fromPeerId.empty()) {
        const ClipboardMonitor::Snapshot snapshot = m_clipboard->snapshot();
        if (snapshot.state == ClipboardMonitor::State::Files &&
            !snapshot.files.empty() && !toPeerId.empty()) {
            const bool hasRemoteOrigin = !snapshot.originPeerId.empty();
            const std::string origin =
                hasRemoteOrigin ? snapshot.originPeerId : std::string();
            const uint32_t sequence = hasRemoteOrigin
                                          ? snapshot.originClipboardSequence
                                          : snapshot.clipboardSequence;
            const std::string dedupeKey = toPeerId + "|" + origin + "|" +
                                          std::to_string(sequence) + "|" +
                                          std::to_string(snapshot.files.size());
            if (dedupeKey == m_lastSentClipboardFilesKey) {
                return;
            }
            m_lastSentClipboardFilesKey = dedupeKey;
            if (m_master->sendFilesMetaTo(toPeerId, snapshot.files,
                                          origin, sequence)) {
                m_lastSentClipboardText.clear();
                m_lastSentClipboardImageKey.clear();
                m_lastSentClipboardRichKey.clear();
                m_filesClipboardSource = origin;
            }
            return;
        }
        if (toPeerId.empty()) {
            return;
        }
        if (snapshot.state == ClipboardMonitor::State::Image &&
            !snapshot.pngData.empty()) {
            if (snapshot.pngData.size() > kMaxClipboardImageBytes) {
                Logger::write(
                    "clipboard: image rejected, bytes=" +
                    std::to_string(snapshot.pngData.size()) +
                    ", limit=" + std::to_string(kMaxClipboardImageBytes));
                return;
            }
            const std::string imageKey =
                std::to_string(snapshot.clipboardSequence) + "|" +
                std::to_string(snapshot.pngData.size());
            if (imageKey != m_lastSentClipboardImageKey) {
                std::string localPeerId;
                {
                    std::lock_guard<std::mutex> layoutLock(m_layoutMutex);
                    localPeerId = m_masterScreenId;
                }
                const std::string transferId = nextClipboardImageTransferId();
                ClipboardImageMeta meta;
                meta.transferId = transferId;
                meta.originPeerId = snapshot.originPeerId.empty()
                                        ? localPeerId
                                        : snapshot.originPeerId;
                meta.originClipboardSequence =
                    snapshot.originPeerId.empty()
                        ? snapshot.clipboardSequence
                        : snapshot.originClipboardSequence;
                meta.width = snapshot.imageWidth;
                meta.height = snapshot.imageHeight;
                meta.totalBytes = static_cast<uint32_t>(snapshot.pngData.size());
                if (saveClipboardImageFile(transferId, snapshot.pngData,
                                           m_clipboardImages[transferId].path)) {
                    m_clipboardImages[transferId].width = meta.width;
                    m_clipboardImages[transferId].height = meta.height;
                    m_clipboardImages[transferId].bytes = meta.totalBytes;
                    if (m_master->sendClipboardImageMetaTo(toPeerId, meta)) {
                        m_lastSentClipboardImageKey = imageKey;
                        m_lastSentClipboardText.clear();
                        m_lastSentClipboardRichKey.clear();
                        m_filesClipboardSource.clear();
                        m_imageClipboardSource.clear();
                        m_richClipboardSource.clear();
                    }
                } else {
                    m_clipboardImages.erase(transferId);
                    Logger::write(
                        "master: failed to save clipboard image cache, bytes=" +
                        std::to_string(snapshot.pngData.size()));
                }
            }
            return;
        }
        if ((snapshot.state == ClipboardMonitor::State::Html &&
             !snapshot.htmlData.empty()) ||
            (snapshot.state == ClipboardMonitor::State::Rtf &&
             !snapshot.rtfData.empty())) {
            const bool isRtf = snapshot.state == ClipboardMonitor::State::Rtf;
            const std::vector<uint8_t>& richData =
                isRtf ? snapshot.rtfData : snapshot.htmlData;
            if (richData.size() > kMaxClipboardRichBytes) {
                Logger::write(
                    "clipboard: rich content rejected, bytes=" +
                    std::to_string(richData.size()) +
                    ", limit=" + std::to_string(kMaxClipboardRichBytes));
                return;
            }
            const std::string richKey =
                std::to_string(snapshot.clipboardSequence) + "|" +
                (isRtf ? std::string("rtf") : std::string("html")) + "|" +
                std::to_string(richData.size());
            if (richKey != m_lastSentClipboardRichKey) {
                std::string localPeerId;
                {
                    std::lock_guard<std::mutex> layoutLock(m_layoutMutex);
                    localPeerId = m_masterScreenId;
                }
                const std::string transferId = nextClipboardRichTransferId();
                ClipboardRichMeta meta;
                meta.transferId = transferId;
                meta.kind = isRtf ? ClipboardRichKind::Rtf
                                  : ClipboardRichKind::Html;
                meta.richBytes = richData.size();
                meta.companionTextBytes = snapshot.text.size();
                meta.originPeerId = snapshot.originPeerId.empty()
                                        ? localPeerId
                                        : snapshot.originPeerId;
                meta.originClipboardSequence =
                    snapshot.originPeerId.empty()
                        ? snapshot.clipboardSequence
                        : snapshot.originClipboardSequence;
                CachedRich& cache = m_clipboardRich[transferId];
                cache.kind = meta.kind;
                cache.richBytes = meta.richBytes;
                cache.textBytes = meta.companionTextBytes;
                if (saveClipboardRichFile(transferId, meta.kind, richData,
                                          snapshot.text, cache.richPath,
                                          cache.textPath)) {
                    if (m_master->sendClipboardRichMetaTo(toPeerId, meta)) {
                        m_lastSentClipboardRichKey = richKey;
                        m_lastSentClipboardText.clear();
                        m_lastSentClipboardImageKey.clear();
                        m_lastSentClipboardFilesKey.clear();
                        m_filesClipboardSource.clear();
                        m_imageClipboardSource.clear();
                        m_richClipboardSource.clear();
                    }
                } else {
                    m_clipboardRich.erase(transferId);
                    Logger::write(
                        "master: failed to save clipboard rich cache, bytes=" +
                        std::to_string(richData.size()));
                }
            }
            return;
        }
        if (snapshot.state != ClipboardMonitor::State::Text ||
            snapshot.text.empty()) {
            return;
        }
        if (snapshot.text == m_lastSentClipboardText) {
            return;
        }
        if (m_master->sendClipboardTextTo(toPeerId, snapshot.text)) {
            m_lastSentClipboardText = snapshot.text;
            m_lastSentClipboardImageKey.clear();
            m_lastSentClipboardRichKey.clear();
            m_filesClipboardSource.clear();
        }
        return;
    }

    if (!m_master->sendClipboardRequestTo(fromPeerId)) {
        m_pendingClipboardTarget.clear();
        return;
    }
    m_pendingClipboardTarget = toPeerId;
}

// 由 MasterNet 在 poll 持锁线程内调用，内部不得再加 m_stateMutex。
void Service::handleClipboardText(const std::string& fromPeerId, const std::string& utf8) {
    (void)fromPeerId;
    if (utf8.empty()) {
        return;
    }
    const std::string target = m_pendingClipboardTarget;
    m_pendingClipboardTarget.clear();
    m_filesClipboardSource.clear();
    if (target.empty()) {
        if (m_clipboard != nullptr) {
            m_clipboard->setTextToClipboard(utf8);
        }
        return;
    }
    if (m_master != nullptr) {
        m_master->sendClipboardTextTo(target, utf8);
    }
}

void Service::handleClipboardImageMeta(const std::string& fromPeerId,
                                       const ClipboardImageMeta& meta) {
    if (meta.transferId.empty() || m_master == nullptr) {
        return;
    }

    std::string origin = meta.originPeerId.empty() ? fromPeerId : meta.originPeerId;
    if (isLocalPeerId(origin)) {
        Logger::write("clipboard: ignored own clipboard image echo, origin=" +
                      origin + ", transferId=" + meta.transferId);
        return;
    }

    ClipboardImageMeta forwarded = meta;
    forwarded.originPeerId = origin;
    const std::string target = m_pendingClipboardTarget;
    m_pendingClipboardTarget.clear();
    m_filesClipboardSource.clear();
    m_imageClipboardSource.clear();
    m_imageClipboardSource = origin;
    if (!target.empty()) {
        m_master->sendClipboardImageMetaTo(target, forwarded);
        return;
    }
    handleRemoteImageBatch(fromPeerId, fromPeerId, forwarded);
}

void Service::handleClipboardRichMeta(const std::string& fromPeerId,
                                      const ClipboardRichMeta& meta) {
    if (meta.transferId.empty() || m_master == nullptr) {
        return;
    }

    std::string origin = meta.originPeerId.empty() ? fromPeerId : meta.originPeerId;
    if (isLocalPeerId(origin)) {
        Logger::write("clipboard: ignored own clipboard rich echo, origin=" +
                      origin + ", transferId=" + meta.transferId);
        return;
    }

    ClipboardRichMeta forwarded = meta;
    forwarded.originPeerId = origin;
    const std::string target = m_pendingClipboardTarget;
    m_pendingClipboardTarget.clear();
    m_filesClipboardSource.clear();
    m_imageClipboardSource.clear();
    m_richClipboardSource = origin;
    if (!target.empty()) {
        m_master->sendClipboardRichMetaTo(target, forwarded);
        return;
    }
    handleRemoteRichBatch(fromPeerId, fromPeerId, forwarded);
}

void Service::handleClipboardFiles(const std::string& fromPeerId,
                                   const std::string& originPeerId,
                                   uint32_t originClipboardSequence,
                                   const std::vector<FileEntry>& files) {
    if (m_master == nullptr || m_clipboard == nullptr || files.empty()) {
        return;
    }

    const std::string target = m_pendingClipboardTarget;
    m_pendingClipboardTarget.clear();
    m_imageClipboardSource.clear();
    if (!target.empty()) {
        if (m_master->sendFilesMetaTo(target, files, originPeerId,
                                      originClipboardSequence)) {
            m_filesClipboardSource = originPeerId.empty() ? fromPeerId : originPeerId;
        }
        return;
    }

    m_filesClipboardSource = originPeerId.empty() ? fromPeerId : originPeerId;
    handleRemoteFilesBatch(fromPeerId, fromPeerId, originPeerId,
                           originClipboardSequence, files);
}

void Service::handleClipboardFilesRequest(const std::string& fromPeerId,
                                          const std::string& fromPeerIp) {
    (void)fromPeerId;
    if (m_master == nullptr || m_fileTransfer == nullptr) {
        Logger::write("master: cannot handle clipboard files request, service unavailable");
        return;
    }

    if (m_filesClipboardSource.empty()) {
        if (m_clipboard != nullptr) {
            m_clipboard->refresh();
            const ClipboardMonitor::Snapshot snapshot = m_clipboard->snapshot();
            if (snapshot.state == ClipboardMonitor::State::Files &&
                !snapshot.files.empty()) {
                Logger::write("master: sending local clipboard files peer=" + fromPeerId +
                              ", ip=" + fromPeerIp + ", count=" +
                              std::to_string(snapshot.files.size()));
                m_fileTransfer->sendAsync(snapshot.files, fromPeerIp,
                                          m_master->peerFilePortByIp(fromPeerIp));
            } else {
                Logger::write("master: no local files available for clipboard files request");
            }
        }
        return;
    }

    const std::string source = m_filesClipboardSource;
    if (!m_master->sendClipboardRequestTo(source, true)) {
        Logger::write("master: cannot relay clipboard files request source=" + source);
        return;
    }
    Logger::write("master: relaying clipboard files from " + source + " to " + fromPeerIp);
    // The relay worker waits for the next inbound TCP session, then forwards it.
    m_fileTransfer->relayAsync(fromPeerIp, m_master->peerFilePortByIp(fromPeerIp));
    // Master is only a relay here; drop the source so a later local paste
    // falls back to Master's own clipboard instead of the previous source.
    m_filesClipboardSource.clear();
}

void Service::handleRemoteFilesBatch(const std::string& sourceId,
                                     const std::string& sourceIp,
                                     const std::string& originPeerId,
                                     uint32_t originClipboardSequence,
                                     const std::vector<FileEntry>& files) {
    if (m_transferQueue == nullptr || m_clipboard == nullptr || files.empty()) {
        return;
    }

    if (m_serviceThreadId != std::this_thread::get_id()) {
        // A transfer worker may pump KCP while waiting. Rebind clipboard and
        // window work to the Service thread instead of creating Win32 state
        // on the worker.
        m_clipboard->postClipboardAction([this, sourceId, sourceIp, originPeerId,
                                          originClipboardSequence, files]() {
            handleRemoteFilesBatch(sourceId, sourceIp, originPeerId,
                                   originClipboardSequence, files);
        });
        return;
    }

    if (isLocalPeerId(originPeerId)) {
        Logger::write("clipboard: ignored own clipboard files echo, origin=" +
                      originPeerId + ", sequence=" +
                      std::to_string(originClipboardSequence));
        return;
    }
    if (originClipboardSequence != 0 && originPeerId == m_lastIgnoredOrigin &&
        originClipboardSequence == m_lastIgnoredSequence) {
        Logger::write("clipboard: ignored previously dismissed clipboard files, origin=" +
                      originPeerId + ", sequence=" +
                      std::to_string(originClipboardSequence));
        return;
    }

    const TransferQueue::OfferResult offer =
        m_transferQueue->offerBatch(files, sourceId, originPeerId,
                                    originClipboardSequence, sourceIp, 0);
    if (offer.restored) {
        if (m_clipboard->setFilesToClipboard(offer.files, offer.originPeerId,
                                             offer.originClipboardSequence)) {
            if (m_pendingConfirmTaskId == offer.taskId) {
                m_pendingConfirmTaskId = 0;
            }
            if (m_confirmWindow) {
                m_confirmWindow->hide();
            }
            Logger::write("clipboard: restored completed remote file batch, task=" +
                          std::to_string(offer.taskId));
            return;
        }
    }

    // The clipboard stays untouched until the user accepts. Installing the
    // delayed CF_HDROP here left a stale item behind after Ignore, and
    // Explorer re-queried it on every right-click.
    Logger::write("clipboard: remote file batch offered, task=" +
                  std::to_string(offer.taskId) + ", created=" +
                  std::to_string(offer.created ? 1 : 0) + ", count=" +
                  std::to_string(files.size()));

    uint64_t totalBytes = 0;
    for (const FileEntry& entry : files) {
        if (!entry.isDir) {
            totalBytes += entry.size;
        }
    }
    if (offer.created) {
        m_pendingConfirmTaskId = offer.taskId;
        m_pendingConfirmOrigin = originPeerId;
        m_pendingConfirmSequence = originClipboardSequence;
        if (m_confirmWindow) {
            const std::string displaySource =
                originPeerId.empty() ? (sourceId.empty() ? sourceIp : sourceId)
                                     : originPeerId;
            m_confirmWindow->show(displaySource, topLevelCount(files), totalBytes,
                                  topLevelPreview(files));
        }
    } else {
        Logger::write("clipboard: reused existing clipboard file task=" +
                      std::to_string(offer.taskId) + ", state=" +
                      std::to_string(static_cast<int>(offer.state)));
    }
}

void Service::handleRemoteImageBatch(const std::string& sourceId,
                                     const std::string& sourceIp,
                                     const ClipboardImageMeta& meta) {
    if (m_transferQueue == nullptr || m_clipboard == nullptr ||
        meta.transferId.empty()) {
        return;
    }

    if (m_serviceThreadId != std::this_thread::get_id()) {
        m_clipboard->postClipboardAction([this, sourceId, sourceIp, meta]() {
            handleRemoteImageBatch(sourceId, sourceIp, meta);
        });
        return;
    }

    if (isLocalPeerId(meta.originPeerId)) {
        Logger::write("clipboard: ignored own clipboard image echo, origin=" +
                      meta.originPeerId + ", transferId=" + meta.transferId);
        return;
    }
    if (meta.originClipboardSequence != 0 &&
        meta.originPeerId == m_lastIgnoredOrigin &&
        meta.originClipboardSequence == m_lastIgnoredSequence) {
        Logger::write(
            "clipboard: ignored previously dismissed clipboard image, origin=" +
            meta.originPeerId + ", sequence=" +
            std::to_string(meta.originClipboardSequence));
        return;
    }

    FileEntry entry;
    entry.name = meta.transferId + ".png";
    entry.size = meta.totalBytes;
    const TransferQueue::OfferResult offer = m_transferQueue->offerBatch(
        {entry}, sourceId, meta.originPeerId, meta.originClipboardSequence,
        sourceIp, 0);
    if (offer.restored) {
        deliverCompletedTransfer(offer.taskId);
        return;
    }

    Logger::write("clipboard: remote image offered, task=" +
                  std::to_string(offer.taskId) + ", created=" +
                  std::to_string(offer.created ? 1 : 0) +
                  ", transferId=" + meta.transferId +
                  ", bytes=" + std::to_string(meta.totalBytes));
    if (offer.created) {
        m_pendingConfirmTaskId = offer.taskId;
        m_pendingConfirmOrigin = meta.originPeerId;
        m_pendingConfirmSequence = meta.originClipboardSequence;
        if (m_confirmWindow) {
            m_confirmWindow->show(
                meta.originPeerId.empty()
                    ? (sourceId.empty() ? sourceIp : sourceId)
                    : meta.originPeerId,
                1, meta.totalBytes,
                {"图片 " + std::to_string(meta.width) + "x" +
                     std::to_string(meta.height)});
        }
    }
}

void Service::handleRemoteRichBatch(const std::string& sourceId,
                                    const std::string& sourceIp,
                                    const ClipboardRichMeta& meta) {
    if (m_transferQueue == nullptr || m_clipboard == nullptr ||
        meta.transferId.empty()) {
        return;
    }

    if (m_serviceThreadId != std::this_thread::get_id()) {
        m_clipboard->postClipboardAction([this, sourceId, sourceIp, meta]() {
            handleRemoteRichBatch(sourceId, sourceIp, meta);
        });
        return;
    }

    if (isLocalPeerId(meta.originPeerId)) {
        Logger::write("clipboard: ignored own clipboard rich echo, origin=" +
                      meta.originPeerId + ", transferId=" + meta.transferId);
        return;
    }
    if (meta.originClipboardSequence != 0 &&
        meta.originPeerId == m_lastIgnoredOrigin &&
        meta.originClipboardSequence == m_lastIgnoredSequence) {
        Logger::write(
            "clipboard: ignored previously dismissed clipboard rich content, origin=" +
            meta.originPeerId + ", sequence=" +
            std::to_string(meta.originClipboardSequence));
        return;
    }

    FileEntry entry;
    entry.name = meta.transferId + richExtension(meta.kind);
    entry.size = meta.richBytes;
    const TransferQueue::OfferResult offer = m_transferQueue->offerBatch(
        {entry}, sourceId, meta.originPeerId, meta.originClipboardSequence,
        sourceIp, 0);
    if (offer.restored) {
        deliverCompletedTransfer(offer.taskId);
        return;
    }

    Logger::write("clipboard: remote rich content offered, task=" +
                  std::to_string(offer.taskId) + ", created=" +
                  std::to_string(offer.created ? 1 : 0) +
                  ", transferId=" + meta.transferId +
                  ", bytes=" + std::to_string(meta.richBytes));
    if (offer.created) {
        m_pendingConfirmTaskId = offer.taskId;
        m_pendingConfirmOrigin = meta.originPeerId;
        m_pendingConfirmSequence = meta.originClipboardSequence;
        if (m_confirmWindow) {
            m_confirmWindow->show(
                meta.originPeerId.empty()
                    ? (sourceId.empty() ? sourceIp : sourceId)
                    : meta.originPeerId,
                1, meta.richBytes,
                {meta.kind == ClipboardRichKind::Rtf
                     ? "富文本 RTF"
                     : "富文本 HTML"});
        }
    }
}

void Service::handleClipboardImageRequest(const std::string& fromPeerId,
                                          const std::string& fromPeerIp,
                                          const std::string& transferId) {
    (void)fromPeerId;
    if (m_master == nullptr || m_fileTransfer == nullptr || transferId.empty()) {
        return;
    }

    if (!m_imageClipboardSource.empty()) {
        if (!m_master->sendClipboardImageRequestTo(m_imageClipboardSource,
                                                   transferId)) {
            Logger::write("master: cannot relay clipboard image request source=" +
                          m_imageClipboardSource);
            return;
        }
        Logger::write("master: relaying clipboard image from " +
                      m_imageClipboardSource + " to " + fromPeerIp);
        m_fileTransfer->relayAsync(fromPeerIp,
                                   m_master->peerFilePortByIp(fromPeerIp));
        m_imageClipboardSource.clear();
        return;
    }

    auto image = m_clipboardImages.find(transferId);
    if (image == m_clipboardImages.end()) {
        Logger::write(
            "master: unknown local clipboard image request, transferId=" +
            transferId);
        return;
    }
    FileEntry entry;
    entry.path = image->second.path;
    entry.name = transferId + ".png";
    entry.size = image->second.bytes;
    Logger::write("master: sending local clipboard image peer=" + fromPeerId +
                  ", ip=" + fromPeerIp + ", transferId=" + transferId +
                  ", bytes=" + std::to_string(entry.size));
    m_fileTransfer->sendAsync({entry}, fromPeerIp,
                              m_master->peerFilePortByIp(fromPeerIp));
}

void Service::handleClipboardRichRequest(const std::string& fromPeerId,
                                         const std::string& fromPeerIp,
                                         const std::string& transferId) {
    (void)fromPeerId;
    if (m_master == nullptr || m_fileTransfer == nullptr || transferId.empty()) {
        return;
    }

    if (!m_richClipboardSource.empty()) {
        if (!m_master->sendClipboardRichRequestTo(m_richClipboardSource,
                                                  transferId)) {
            Logger::write("master: cannot relay clipboard rich request source=" +
                          m_richClipboardSource);
            return;
        }
        Logger::write("master: relaying clipboard rich content from " +
                      m_richClipboardSource + " to " + fromPeerIp);
        m_fileTransfer->relayAsync(fromPeerIp,
                                   m_master->peerFilePortByIp(fromPeerIp));
        m_richClipboardSource.clear();
        return;
    }

    auto rich = m_clipboardRich.find(transferId);
    if (rich == m_clipboardRich.end()) {
        Logger::write(
            "master: unknown local clipboard rich request, transferId=" +
            transferId);
        return;
    }
    FileEntry richEntry;
    richEntry.path = rich->second.richPath;
    richEntry.name = transferId + richExtension(rich->second.kind);
    richEntry.size = rich->second.richBytes;
    FileEntry textEntry;
    textEntry.path = rich->second.textPath;
    textEntry.name = transferId + ".txt";
    textEntry.size = rich->second.textBytes;
    Logger::write("master: sending local clipboard rich content peer=" +
                  fromPeerId + ", ip=" + fromPeerIp + ", transferId=" +
                  transferId + ", bytes=" +
                  std::to_string(richEntry.size + textEntry.size));
    m_fileTransfer->sendAsync({richEntry, textEntry}, fromPeerIp,
                              m_master->peerFilePortByIp(fromPeerIp));
}

TransferQueue::ExecuteResult Service::executeRemoteFileTransfer(
    const TransferQueue::Task& task, const std::atomic<bool>& cancel) {
    if (cancel.load()) {
        return {};
    }
    if (!task.metadata.empty() && isImageTransferFile(task.metadata.front())) {
        const std::string transferId = task.metadata.front().name.substr(
            0, task.metadata.front().name.size() - 4);
        if (m_role == "slave" && m_slave != nullptr) {
            TransferQueue::ExecuteResult result;
            result.outcome = TransferQueue::ExecuteResult::Outcome::Completed;
            result.files = m_slave->requestClipboardImage(transferId, cancel);
            return result;
        }
        if (m_role == "master" && m_master != nullptr && m_fileTransfer != nullptr) {
            TransferQueue::ExecuteResult result;
            result.outcome = TransferQueue::ExecuteResult::Outcome::Completed;
            result.files = requestRemoteImageFromPeer(task.sourceId, transferId, cancel);
            return result;
        }
    }
    if (!task.metadata.empty() && isRichTransferFile(task.metadata.front())) {
        const FileEntry& entry = task.metadata.front();
        const size_t dot = entry.name.find_last_of('.');
        const std::string transferId = entry.name.substr(0, dot);
        if (m_role == "slave" && m_slave != nullptr) {
            TransferQueue::ExecuteResult result;
            result.outcome = TransferQueue::ExecuteResult::Outcome::Completed;
            result.files = m_slave->requestClipboardRich(transferId, cancel);
            return result;
        }
        if (m_role == "master" && m_master != nullptr && m_fileTransfer != nullptr) {
            TransferQueue::ExecuteResult result;
            result.outcome = TransferQueue::ExecuteResult::Outcome::Completed;
            result.files = requestRemoteRichFromPeer(task.sourceId, transferId, cancel);
            return result;
        }
    }
    if (m_role == "slave" && m_slave != nullptr) {
        {
            std::lock_guard<std::mutex> lock(m_activeFileMutex);
            m_activeFileTaskId = task.id;
        }
        if (!m_slave->beginClipboardFilesRequest()) {
            std::lock_guard<std::mutex> lock(m_activeFileMutex);
            m_activeFileTaskId = 0;
            return {};
        }
        return waitForExternalFileTransfer(task.id, cancel);
    }
    if (m_role == "master" && m_master != nullptr && m_fileTransfer != nullptr) {
        {
            std::lock_guard<std::mutex> lock(m_activeFileMutex);
            m_activeFileTaskId = task.id;
        }
        if (!beginRemoteFilesFromPeer(task.sourceId)) {
            std::lock_guard<std::mutex> lock(m_activeFileMutex);
            m_activeFileTaskId = 0;
            return {};
        }
        return waitForExternalFileTransfer(task.id, cancel);
    }
    Logger::write("transfer queue: no active role for task=" +
                  std::to_string(task.id));
    return {};
}

TransferQueue::ExecuteResult Service::waitForExternalFileTransfer(
    uint64_t taskId, const std::atomic<bool>& cancel) {
    const uint64_t deadline = GetTickCount64() + 30000;
    while (!cancel.load()) {
        if (m_fileTransfer && m_fileTransfer->hasReceiveSession()) {
            TransferQueue::ExecuteResult result;
            result.outcome = TransferQueue::ExecuteResult::Outcome::ExternalPending;
            return result;
        }
        if (GetTickCount64() >= deadline) {
            break;
        }
        Sleep(20);
    }

    Logger::write(cancel.load()
                      ? "transfer queue: clipboard files request cancelled, task=" +
                            std::to_string(taskId)
                      : "transfer queue: timed out waiting for file connection, task=" +
                            std::to_string(taskId));
    std::lock_guard<std::mutex> lock(m_activeFileMutex);
    if (m_activeFileTaskId == taskId) {
        m_activeFileTaskId = 0;
    }
    return {};
}

void Service::handleTransferQueueEvent(const TransferQueue::Task& task) {
    switch (task.state) {
        case TransferQueue::State::Completed:
            if (m_clipboard != nullptr) {
                const uint64_t taskId = task.id;
                m_clipboard->postClipboardAction([this, taskId]() {
                    deliverCompletedTransfer(taskId);
                });
            }
            break;
        case TransferQueue::State::Failed:
            Logger::write("transfer queue: remote file task failed, task=" +
                          std::to_string(task.id));
            if (m_clipboard != nullptr) {
                m_clipboard->postClipboardAction([this]() {
                    m_clipboard->clearPendingRemoteFiles();
                });
            }
            if (m_pendingConfirmTaskId == task.id) {
                m_pendingConfirmTaskId = 0;
                if (m_confirmWindow) {
                    m_confirmWindow->hide();
                }
            }
            break;
        case TransferQueue::State::Cancelled:
            Logger::write("transfer queue: remote file task cancelled, task=" +
                          std::to_string(task.id));
            if (m_clipboard != nullptr) {
                m_clipboard->postClipboardAction([this]() {
                    m_clipboard->clearPendingRemoteFiles();
                });
            }
            if (m_pendingConfirmTaskId == task.id) {
                m_pendingConfirmTaskId = 0;
                if (m_confirmWindow) {
                    m_confirmWindow->hide();
                }
            }
            break;
        default:
            break;
    }
}

void Service::handleReceiveCompleted(bool success,
                                     std::vector<FileEntry> files) {
    uint64_t taskId = 0;
    {
        std::lock_guard<std::mutex> lock(m_activeFileMutex);
        taskId = m_activeFileTaskId;
        m_activeFileTaskId = 0;
    }
    if (taskId == 0) {
        return;
    }

    // The non-blocking file path consumes the receive result directly; leaving
    // it behind could satisfy the next image/rich-text pull request too early.
    if (m_fileTransfer) {
        const FileTransferManager::ReceiveResult discarded =
            m_fileTransfer->consumeReceiveResult();
        (void)discarded;
    }

    if (m_transferQueue == nullptr) {
        return;
    }
    if (success && !files.empty()) {
        m_transferQueue->completeExternal(taskId, std::move(files));
    } else {
        m_transferQueue->failExternal(taskId);
    }
}

void Service::cancelExternalFileTransfer(const TransferQueue::Task& task) {
    bool isActive = false;
    {
        std::lock_guard<std::mutex> lock(m_activeFileMutex);
        isActive = m_activeFileTaskId == task.id;
    }
    if (isActive && m_fileTransfer) {
        m_fileTransfer->cancelActiveReceive();
    }
}

ClipboardMonitor::RemotePasteResult Service::gateRemotePaste(
    const std::vector<FileEntry>& metadata) {
    if (m_transferQueue == nullptr) {
        return ClipboardMonitor::RemotePasteResult{};
    }

    const TransferQueue::PasteDecision decision =
        m_transferQueue->decidePaste(metadata);
    if (decision.action == TransferQueue::PasteDecision::Action::Wait) {
        if (m_confirmWindow) {
            m_confirmWindow->bringToFront();
        }
        ClipboardMonitor::RemotePasteResult result;
        result.decision = ClipboardMonitor::RemotePasteDecision::Wait;
        return result;
    }
    if (decision.action == TransferQueue::PasteDecision::Action::Ready) {
        ClipboardMonitor::RemotePasteResult result;
        result.decision = ClipboardMonitor::RemotePasteDecision::Ready;
        result.files = decision.files;
        return result;
    }
    return ClipboardMonitor::RemotePasteResult{};
}

void Service::deliverCompletedTransfer(uint64_t taskId) {
    if (m_transferQueue == nullptr || m_clipboard == nullptr) {
        return;
    }
    const TransferQueue::CompletedFiles completed =
        m_transferQueue->takeCompletedFiles(taskId);
    const std::vector<FileEntry>& files = completed.files;
    if (files.empty()) {
        return;
    }
    if (files.size() == 1 && isImageTransferFile(files.front())) {
        const std::vector<uint8_t> png =
            readFileBytes(files.front().path, kMaxClipboardImageBytes);
        if (png.empty() || !m_clipboard->setImageToClipboard(png)) {
            Logger::write("clipboard: failed to install completed image, task=" +
                          std::to_string(taskId));
            return;
        }
        DeleteFileA(files.front().path.c_str());
    } else if (files.size() == 2 && isRichTransferFile(files.front()) &&
               files.back().name.size() > 4 &&
               files.back().name.compare(files.back().name.size() - 4, 4,
                                         ".txt") == 0) {
        const FileEntry& richEntry = files.front();
        const std::vector<uint8_t> rich =
            readFileBytes(richEntry.path, kMaxClipboardRichBytes);
        const std::vector<uint8_t> text =
            readFileBytes(files.back().path, kMaxClipboardRichBytes);
        if (rich.empty()) {
            Logger::write("clipboard: failed to read completed rich content, task=" +
                          std::to_string(taskId));
            return;
        }
        const std::string companionText(text.begin(), text.end());
        const bool rtf = richEntry.name.compare(richEntry.name.size() - 4, 4,
                                                ".rtf") == 0;
        const bool installed =
            rtf ? m_clipboard->setRtfToClipboard(rich, companionText)
                : m_clipboard->setHtmlToClipboard(rich, companionText);
        if (!installed) {
            Logger::write("clipboard: failed to install completed rich content, task=" +
                          std::to_string(taskId));
            return;
        }
        DeleteFileA(richEntry.path.c_str());
        DeleteFileA(files.back().path.c_str());
    } else if (!m_clipboard->setFilesToClipboard(files, completed.originPeerId,
                                                 completed.originClipboardSequence)) {
        Logger::write("clipboard: failed to install completed transfer, task=" +
                      std::to_string(taskId));
        return;
    }
    if (m_pendingConfirmTaskId == taskId) {
        m_pendingConfirmTaskId = 0;
        if (m_confirmWindow) {
            m_confirmWindow->hide();
        }
    }
}

void Service::ignorePendingTransfer(uint64_t taskId) {
    m_pendingConfirmTaskId = 0;
    m_lastIgnoredOrigin = m_pendingConfirmOrigin;
    m_lastIgnoredSequence = m_pendingConfirmSequence;
    m_pendingConfirmOrigin.clear();
    m_pendingConfirmSequence = 0;
    if (m_transferQueue && taskId != 0) {
        m_transferQueue->cancel(taskId);
    }
    if (m_clipboard) {
        m_clipboard->postClipboardAction([this]() {
            m_clipboard->clearPendingRemoteFiles();
        });
    }
}

void Service::handleTransferProgress(const FileTransferManager::Status& status) {
    if (m_progressWindow == nullptr) {
        return;
    }
    if (!status.active) {
        m_progressWindow->hide();
        return;
    }

    TransferProgressWindow::Snapshot snapshot;
    snapshot.active = true;
    snapshot.fileName = status.fileName;
    snapshot.fileIndex = status.fileIndex;
    snapshot.fileCount = status.fileCount;
    snapshot.fileTransferred = status.fileTransferred;
    snapshot.fileTotal = status.fileTotal;
    snapshot.batchTransferred = status.transferred;
    snapshot.batchTotal = status.total;
    if (m_transferQueue != nullptr) {
        const TransferQueue::QueueStats stats = m_transferQueue->stats();
        snapshot.queueRemaining = stats.remainingCount;
    }
    m_progressWindow->update(snapshot);
}

bool Service::isLocalPeerId(const std::string& peerId) {
    if (peerId.empty()) {
        return false;
    }
    if (m_role == "master") {
        std::lock_guard<std::mutex> lock(m_layoutMutex);
        return iequals(peerId, m_masterScreenId);
    }
    if (m_role == "slave" && m_slave != nullptr) {
        return iequals(peerId, m_slave->peerId());
    }
    return false;
}

bool Service::beginRemoteFilesFromPeer(const std::string& peerId) {
    if (m_master == nullptr || m_fileTransfer == nullptr || peerId.empty()) {
        Logger::write("master: cannot request remote files, service unavailable");
        return false;
    }

    if (!m_master->sendClipboardRequestTo(peerId, true)) {
        Logger::write("master: cannot request clipboard files from " + peerId);
        return false;
    }
    return true;
}

std::vector<FileEntry> Service::requestRemoteImageFromPeer(
    const std::string& peerId, const std::string& transferId,
    const std::atomic<bool>& cancel) {
    if (m_master == nullptr || m_fileTransfer == nullptr || peerId.empty() ||
        transferId.empty()) {
        Logger::write("master: cannot request remote image, service unavailable");
        return {};
    }

    const uint64_t token = m_fileTransfer->beginReceiveWait();
    if (!m_master->sendClipboardImageRequestTo(peerId, transferId)) {
        Logger::write("master: cannot request clipboard image from " + peerId +
                      ", transferId=" + transferId);
        return {};
    }

    const uint64_t deadline = GetTickCount64() + 30000;
    while (!cancel.load() && GetTickCount64() < deadline) {
        if (m_fileTransfer->waitForReceive(token, 20)) {
            const FileTransferManager::ReceiveResult result =
                m_fileTransfer->consumeReceiveResult();
            return result.success ? result.files : std::vector<FileEntry>{};
        }
    }
    Logger::write(cancel.load()
                      ? "master: clipboard image request cancelled"
                      : "master: timed out waiting for clipboard image from " +
                            peerId);
    return {};
}

std::vector<FileEntry> Service::requestRemoteRichFromPeer(
    const std::string& peerId, const std::string& transferId,
    const std::atomic<bool>& cancel) {
    if (m_master == nullptr || m_fileTransfer == nullptr || peerId.empty() ||
        transferId.empty()) {
        Logger::write("master: cannot request remote rich content, service unavailable");
        return {};
    }

    const uint64_t token = m_fileTransfer->beginReceiveWait();
    if (!m_master->sendClipboardRichRequestTo(peerId, transferId)) {
        Logger::write("master: cannot request clipboard rich content from " +
                      peerId + ", transferId=" + transferId);
        return {};
    }

    const uint64_t deadline = GetTickCount64() + 30000;
    while (!cancel.load() && GetTickCount64() < deadline) {
        if (m_fileTransfer->waitForReceive(token, 20)) {
            const FileTransferManager::ReceiveResult result =
                m_fileTransfer->consumeReceiveResult();
            return result.success ? result.files : std::vector<FileEntry>{};
        }
    }
    Logger::write(cancel.load()
                      ? "master: clipboard rich request cancelled"
                      : "master: timed out waiting for clipboard rich content from " +
                            peerId);
    return {};
}

bool Service::updateLayout(const nlohmann::json& layoutJson) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (m_role != "master") {
        return false;
    }
    ScreenLayout layout;
    if (!layout.loadFromJson(layoutJson)) {
        std::cerr << "update_layout: invalid layout\n";
        Logger::write("update_layout: invalid layout rejected");
        return false;
    }
    std::string masterId;
    if (!resolveMasterScreen(layout, masterId)) {
        std::cerr << "update_layout: cannot identify master screen\n";
        Logger::write("update_layout: cannot identify master screen");
        return false;
    }
    Logger::write("master: layout updated");
    if (m_master) {
        std::vector<MasterNet::PeerInit> peerInits;
        for (const ScreenNode& node : layout.nodes()) {
            if (node.id != masterId && !node.ip.empty()) {
                peerInits.push_back({node.id, node.ip,
                                     static_cast<uint16_t>(node.port > 0 ? node.port : 19881)});
            }
        }
        m_master->remapPeerIps(peerInits);
    }
    // Routing data only; peer connections and active input state are untouched.
    {
        std::lock_guard<std::mutex> layoutLock(m_layoutMutex);
        m_layout = std::move(layout);
        m_masterScreenId = masterId;
    }
    persistMasterLayout(m_layout);
    return true;
}

void Service::reauthenticatePeers() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (m_role == "master" && m_master) {
        m_master->reauthenticate();
        Logger::write("master: peers queued for preshared-key re-authentication");
    } else if (m_role == "slave" && m_slave) {
        m_slave->disconnectAll();
    }
}

bool Service::resolveMasterScreen(const ScreenLayout& layout, std::string& masterId) const {
    const std::string localHost = localHostnameLower();
    if (!localHost.empty()) {
        for (const ScreenNode& node : layout.nodes()) {
            if (!node.hostname.empty() && iequals(node.hostname, localHost)) {
                masterId = node.id;
                return true;
            }
        }
    }
    ScreenNode node;
    if (layout.findNode("master", node)) {
        masterId = node.id;
        return true;
    }
    const std::vector<ScreenNode> allNodes = layout.nodes();
    if (allNodes.size() == 1) {
        masterId = allNodes.front().id;
        return true;
    }
    return false;
}

InputCapture::RouteTarget Service::lookupRouteForCapture(const std::string& screenId,
                                                         Edge edge, float ratio) {
    InputCapture::RouteTarget result;
    ScreenRouting routing;
    if (!m_layout.findRouting(screenId, routing)) {
        return result;
    }
    EdgeSegment segment;
    if (!lookupRoute(routing, edge, ratio, segment)) {
        return result;
    }

    bool isMaster = false;
    {
        std::lock_guard<std::mutex> lock(m_layoutMutex);
        isMaster = segment.toId == m_masterScreenId;
    }

    uint16_t targetW = 0;
    uint16_t targetH = 0;
    if (isMaster) {
        targetW = static_cast<uint16_t>(GetSystemMetrics(SM_CXSCREEN));
        targetH = static_cast<uint16_t>(GetSystemMetrics(SM_CYSCREEN));
    } else {
        ScreenNode node;
        if (!m_layout.findNode(segment.toId, node)) {
            return result;
        }
        targetW = static_cast<uint16_t>(node.screenW);
        targetH = static_cast<uint16_t>(node.screenH);
        // Prefer the resolution reported by the peer handshake.
        if (m_master) {
            const MasterNet::ScreenSize actual = m_master->peerScreenById(segment.toId);
            if (actual.valid) {
                targetW = actual.width;
                targetH = actual.height;
            }
        }
    }
    if (targetW == 0 || targetH == 0) {
        return result;
    }

    const TargetPos pos = calcTargetPos(segment, ratio, targetW, targetH);
    result.found = true;
    result.isMaster = isMaster;
    result.peerId = segment.toId;
    result.toEdge = segment.toEdge;
    result.entryX = static_cast<uint16_t>(
        std::clamp(pos.x, 0, static_cast<int>(targetW) - 1));
    result.entryY = static_cast<uint16_t>(
        std::clamp(pos.y, 0, static_cast<int>(targetH) - 1));
    result.width = targetW;
    result.height = targetH;
    return result;
}

nlohmann::json Service::handleIpc(const nlohmann::json& command) {
    const std::string type = command.value("type", std::string());
    if (type == "get_status") {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        return getStatusLocked();
    }
    if (type == "export_key") {
        const std::string key = SecurityKeyStore::load();
        return {{"type", "key"}, {"key", key}};
    }
    if (type == "import_key" || type == "regenerate_key") {
        bool ok = false;
        if (type == "import_key") {
            ok = SecurityKeyStore::importKey(command.value("key", std::string()));
        } else {
            ok = !SecurityKeyStore::regenerate().empty();
        }
        if (!ok) {
            return {{"type", "error"},
                    {"message", "invalid key or empty key cannot be saved"}};
        }
        if (m_role != "idle") {
            std::lock_guard<std::mutex> lock(m_commandMutex);
            m_ipcCommands.push({{"type", "reauthenticate"}});
        }
        return {{"type", "ok"}, {"accepted", type}};
    }
    if (type == "set_kcp_params") {
        KcpParameters parameters;
        if (!parseKcpParameters(command, parameters)) {
            return {{"type", "error"}, {"message", "invalid KCP parameters"}};
        }
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_kcpOverride = true;
            m_kcpOverrideParameters = parameters;
            if (m_master) {
                m_master->setKcpOverride(parameters);
            }
            if (m_slave) {
                m_slave->setKcpOverride(parameters);
            }
        }
        Logger::write("service: manual KCP parameters applied");
        return {{"type", "ok"}, {"accepted", type}};
    }
    if (type == "start_master" || type == "start_slave" || type == "stop" ||
        type == "update_layout" || type == "exit") {
        {
            std::lock_guard<std::mutex> lock(m_commandMutex);
            m_ipcCommands.push(command);
        }
        return {{"type", "ok"}, {"accepted", type}};
    }
    return {{"type", "error"}, {"message", "unknown command"}};
}

nlohmann::json Service::getStatusLocked() {
    nlohmann::json peers = nlohmann::json::array();
    if (m_role == "master" && m_master) {
        const auto layoutConfigured = [this](const MasterNet::Peer& peer) {
            ScreenNode node;
            return m_layout.findNode(peer.id, node) && node.ip == peer.ip;
        };
        for (const MasterNet::Peer& peer : m_master->peers()) {
            peers.push_back({
                {"id", peer.id},
                {"endpoint", endpointToString(peer.endpoint)},
                {"connected", peer.connected},
                {"screenW", peer.screenW},
                {"screenH", peer.screenH},
                {"latencyMs", peer.lastRttMs},
                {"networkType", peer.networkType},
                {"kcp", kcpParametersToJson(peer.kcpParameters)},
                {"layoutConfigured", layoutConfigured(peer)},
                {"ip", peer.ip},
            });
        }
    } else if (m_role == "slave" && m_slave) {
        for (const SlaveNet::Peer& peer : m_slave->peers()) {
            peers.push_back({
                {"id", peer.id},
                {"endpoint", endpointToString(peer.endpoint)},
                {"ip", endpointAddressToString(peer.endpoint)},
                {"connected", true},
                {"connectedSinceMs", peer.connectedSinceMs},
                {"kcp", kcpParametersToJson(peer.kcpParameters)},
            });
        }
    }

    nlohmann::json status{
        {"type", "status"},
        {"role", m_role},
        {"running", m_role != "idle"},
        {"peers", std::move(peers)},
    };
    if (m_role == "master" && m_master) {
        status["focus"] = m_master->focusedPeerId();
    }

    if (m_role == "master" && m_master) {
        bool crossNetwork = false;
        KcpParameters active = m_kcpOverride ? m_kcpOverrideParameters : kLanKcpParameters;
        for (const MasterNet::Peer& peer : m_master->peers()) {
            crossNetwork = crossNetwork || peer.networkType != "lan";
            if (!m_kcpOverride) {
                active = peer.kcpParameters;
            }
        }
        status["network"] = {
            {"type", crossNetwork ? "cross" : "lan"},
            {"kcp", kcpParametersToJson(active)},
            {"manualKcp", m_kcpOverride},
        };
    } else if (m_role == "slave" && m_slave) {
        const SlaveNet::MouseStats stats = m_slave->mouseStats();
        const uint64_t total = stats.received + stats.lost;
        const double lossRate = total > 0
            ? static_cast<double>(stats.lost) / static_cast<double>(total) : 0.0;
        status["network"] = {
            {"type", m_kcpOverride ? "manual" : "lan"},
            {"manualKcp", m_kcpOverride},
            {"mouseLossRate", lossRate},
            {"mouseReceived", stats.received},
            {"mouseLost", stats.lost},
            {"kcp", kcpParametersToJson(m_kcpOverride ? m_kcpOverrideParameters
                                                       : kLanKcpParameters)},
        };
    }

    status["security"] = {
        // Read-only: a status poll must never auto-generate a key, otherwise
        // the configured empty-key mode would break on the next GUI refresh.
        {"keyConfigured", !SecurityKeyStore::load().empty()},
    };

    std::string clipboardState = "empty";
    if (m_clipboard) {
        const ClipboardMonitor::Snapshot snapshot = m_clipboard->snapshot();
        if (snapshot.state == ClipboardMonitor::State::Text) {
            clipboardState = "text";
        } else if (snapshot.state == ClipboardMonitor::State::Files) {
            clipboardState = "files";
        } else if (snapshot.state == ClipboardMonitor::State::Image) {
            clipboardState = "image";
        } else if (snapshot.state == ClipboardMonitor::State::Html) {
            clipboardState = "html";
        } else if (snapshot.state == ClipboardMonitor::State::Rtf) {
            clipboardState = "rtf";
        }
    }

    FileTransferManager::Status transferStatus;
    if (m_fileTransfer) {
        transferStatus = m_fileTransfer->status();
    }
    status["clipboard"] = {{"state", clipboardState}};
    status["transfer"] = {
        {"active", transferStatus.active},
        {"file", transferStatus.fileName},
        {"transferred", transferStatus.transferred},
        {"total", transferStatus.total},
    };
    return status;
}

void Service::processIpcCommands() {
    std::queue<nlohmann::json> commands;
    {
        std::lock_guard<std::mutex> lock(m_commandMutex);
        commands.swap(m_ipcCommands);
    }

    while (!commands.empty()) {
        const nlohmann::json command = commands.front();
        commands.pop();
        const std::string type = command.value("type", std::string());
        if (type == "start_master") {
            const auto layoutIt = command.find("layout");
            if (layoutIt != command.end() && layoutIt->is_object()) {
                applySecurityKey(command.value("securityKey", std::string()));
                const std::string bindIp = command.value("bindIp", "0.0.0.0");
                const uint16_t port = static_cast<uint16_t>(command.value("port", 19881));
                startMaster(*layoutIt, bindIp, port);
            } else {
                const std::string ip = command.value("ip", m_masterIp);
                applySecurityKey(command.value("securityKey", std::string()));
                startMaster(ip, static_cast<uint16_t>(command.value("port", 19881)));
            }
        } else if (type == "start_slave") {
            applySecurityKey(command.value("securityKey", std::string()));
            startSlave(static_cast<uint16_t>(command.value("localPort", 19881)),
                       command.value("masterIp", std::string()),
                       static_cast<uint16_t>(command.value("masterPort", 19881)));
        } else if (type == "stop") {
            stopMode();
        } else if (type == "exit") {
            stopMode();
            m_running = false;
        } else if (type == "update_layout") {
            const auto layoutIt = command.find("layout");
            if (layoutIt == command.end() || !layoutIt->is_object()) {
                std::cerr << "update_layout: missing layout object\n";
            } else if (!updateLayout(*layoutIt)) {
                std::cerr << "update_layout: rejected\n";
            }
        } else if (type == "reauthenticate") {
            reauthenticatePeers();
        }
    }
}

void Service::run() {
    m_running = true;
    m_serviceThreadId = std::this_thread::get_id();
    MSG message{};
    while (m_running) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                m_running = false;
            } else {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (!m_running) {
            break;
        }

        processIpcCommands();
        const uint64_t nowMs = GetTickCount64();
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            if (m_role == "master" && m_master) {
                m_master->poll(nowMs);
            } else if (m_role == "slave" && m_slave) {
                m_slave->poll(nowMs);
            }
        }
        Sleep(5);
    }
}

}  // namespace synccontrol
