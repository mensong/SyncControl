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

bool filesEqual(const std::vector<FileEntry>& left, const std::vector<FileEntry>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t index = 0; index < left.size(); ++index) {
        const FileEntry& a = left[index];
        const FileEntry& b = right[index];
        if (a.path != b.path || a.name != b.name || a.size != b.size || a.isDir != b.isDir) {
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

}  // namespace

Service::Service() {
    m_ipc = std::make_unique<IpcServer>([this](const nlohmann::json& command) {
        return handleIpc(command);
    });
    m_ipc->start();
}

Service::~Service() {
    stopMode();
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
    if (!m_fileTransfer->start(bindIp.empty() ? "0.0.0.0" : bindIp,
                               localPort != 0 ? localPort : 19881)) {
        m_master->stop();
        m_role = "idle";
        std::cerr << "start_master: file transfer listener failed\n";
        return false;
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
    m_master->onClipboardFiles = [this](const std::string& fromPeerId,
                                        const std::vector<FileEntry>& files) {
        handleClipboardFiles(fromPeerId, files);
    };
    m_master->onClipboardFilesRequest = [this](const std::string& fromPeerId,
                                               const std::string& fromPeerIp) {
        handleClipboardFilesRequest(fromPeerId, fromPeerIp);
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
    m_clipboard->setFileRequestHandler([this](const std::vector<FileEntry>& metadata) {
        return requestFilesForMasterRender(metadata);
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
    m_clipboard->setFileRequestHandler([this](const std::vector<FileEntry>& metadata) {
        return requestFilesForSlaveRender(metadata);
    });
    if (!m_slave) {
        m_slave = std::make_unique<SlaveNet>();
    }
    m_slave->setClipboardMonitor(m_clipboard.get());
    if (!m_fileTransfer) {
        m_fileTransfer = std::make_unique<FileTransferManager>();
    }
    if (!m_fileTransfer->start("0.0.0.0", localPort != 0 ? localPort : 19881)) {
        m_role = "idle";
        std::cerr << "start_slave: file transfer listener failed\n";
        return false;
    }
    m_slave->setFileTransfer(m_fileTransfer.get());
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
    }
    if (m_clipboard) {
        m_clipboard->stop();
    }
    m_pendingClipboardTarget.clear();
    m_lastSentClipboardText.clear();
    m_lastSentClipboardFiles.clear();
    m_filesClipboardSource.clear();
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
        m_clipboard->refresh();
        const ClipboardMonitor::Snapshot snapshot = m_clipboard->snapshot();
        if (snapshot.state == ClipboardMonitor::State::Files &&
            !snapshot.files.empty() && !toPeerId.empty()) {
            if (filesEqual(snapshot.files, m_lastSentClipboardFiles)) {
                return;
            }
            if (m_master->sendFilesMetaTo(toPeerId, snapshot.files)) {
                m_lastSentClipboardFiles = snapshot.files;
                m_lastSentClipboardText.clear();
                m_filesClipboardSource.clear();
            }
            return;
        }
        if (snapshot.state != ClipboardMonitor::State::Text ||
            snapshot.text.empty() || toPeerId.empty()) {
            return;
        }
        if (snapshot.text == m_lastSentClipboardText) {
            return;
        }
        if (m_master->sendClipboardTextTo(toPeerId, snapshot.text)) {
            m_lastSentClipboardText = snapshot.text;
            m_lastSentClipboardFiles.clear();
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

void Service::handleClipboardFiles(const std::string& fromPeerId,
                                   const std::vector<FileEntry>& files) {
    if (m_master == nullptr || m_clipboard == nullptr || files.empty()) {
        return;
    }

    const std::string target = m_pendingClipboardTarget;
    m_pendingClipboardTarget.clear();
    if (!target.empty()) {
        if (m_master->sendFilesMetaTo(target, files)) {
            m_filesClipboardSource = fromPeerId;
        }
        return;
    }

    m_filesClipboardSource = fromPeerId;
    m_clipboard->setPendingFilesMeta(files);
}

void Service::handleClipboardFilesRequest(const std::string& fromPeerId,
                                          const std::string& fromPeerIp) {
    (void)fromPeerId;
    if (m_master == nullptr || m_fileTransfer == nullptr) {
        return;
    }

    if (m_filesClipboardSource.empty()) {
        if (m_clipboard != nullptr) {
            m_clipboard->refresh();
            const ClipboardMonitor::Snapshot snapshot = m_clipboard->snapshot();
            if (snapshot.state == ClipboardMonitor::State::Files &&
                !snapshot.files.empty()) {
                m_fileTransfer->sendAsync(snapshot.files, fromPeerIp,
                                          m_master->peerFilePortByIp(fromPeerIp));
            }
        }
        return;
    }

    const std::string source = m_filesClipboardSource;
    if (!m_master->sendClipboardRequestTo(source, true)) {
        return;
    }
    // The relay worker waits for the next inbound TCP session, then forwards it.
    m_fileTransfer->relayAsync(fromPeerIp, m_master->peerFilePortByIp(fromPeerIp));
    // Master is only a relay here; drop the source so a later local paste
    // falls back to Master's own clipboard instead of the previous source.
    m_filesClipboardSource.clear();
}

std::vector<FileEntry> Service::requestFilesForSlaveRender(
    const std::vector<FileEntry>& metadata) {
    (void)metadata;
    return m_slave != nullptr ? m_slave->requestClipboardFiles() : std::vector<FileEntry>{};
}

std::vector<FileEntry> Service::requestFilesForMasterRender(
    const std::vector<FileEntry>& metadata) {
    (void)metadata;
    if (m_master == nullptr || m_fileTransfer == nullptr) {
        return {};
    }

    if (m_filesClipboardSource.empty()) {
        if (m_clipboard != nullptr) {
            m_clipboard->refresh();
            const ClipboardMonitor::Snapshot snapshot = m_clipboard->snapshot();
            if (snapshot.state == ClipboardMonitor::State::Files) {
                return snapshot.files;
            }
        }
        return {};
    }

    const std::string source = m_filesClipboardSource;
    const uint64_t token = m_fileTransfer->beginReceiveWait();
    if (!m_master->sendClipboardRequestTo(source, true)) {
        return {};
    }

    const uint64_t deadline = GetTickCount64() + 30000;
    while (GetTickCount64() < deadline) {
        if (m_fileTransfer->waitForReceive(token, 20)) {
            const FileTransferManager::ReceiveResult result =
                m_fileTransfer->consumeReceiveResult();
            return result.success ? result.files : std::vector<FileEntry>{};
        }
        // Flush KCP and service heartbeats while this render callback blocks.
        m_master->poll(GetTickCount64());
    }
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
        type == "update_layout") {
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
