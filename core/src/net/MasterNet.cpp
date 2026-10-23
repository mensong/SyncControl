#include "net/MasterNet.h"

#include <Windows.h>

#include <algorithm>
#include <json.hpp>

#include "core/Logger.h"
#include "core/Security.h"
#include "net/Protocol.h"

namespace synccontrol {
namespace {

constexpr uint16_t kKcpConversation = 1;
constexpr uint64_t kHandshakeRetryMs = 1000;
constexpr uint64_t kHeartbeatIntervalMs = 1000;
constexpr uint64_t kRawHeartbeatIntervalMs = 1000;
constexpr uint64_t kKcpLivenessMs = 4000;
constexpr uint64_t kPeerTimeoutMs = 8000;
constexpr uint64_t kUdpWarningMs = 5000;

bool sameEndpoint(const sockaddr_in& left, const sockaddr_in& right) {
    return left.sin_family == right.sin_family &&
           left.sin_addr.s_addr == right.sin_addr.s_addr &&
           left.sin_port == right.sin_port;
}

uint16_t readU16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8);
}

bool sameIpv4Subnet(const std::string& peerIp) {
    in_addr peerAddress{};
    if (InetPtonA(AF_INET, peerIp.c_str(), &peerAddress) != 1) {
        return false;
    }

    char hostName[256]{};
    if (gethostname(hostName, sizeof(hostName)) != 0) {
        return false;
    }
    const hostent* local = gethostbyname(hostName);
    if (local == nullptr || local->h_addrtype != AF_INET) {
        return false;
    }

    for (char** address = local->h_addr_list; *address != nullptr; ++address) {
        const in_addr localAddress = *reinterpret_cast<in_addr*>(*address);
        // P8 uses the common /24 heuristic for the local LAN; routed VPN or
        // multi-address hosts will fall back to the conservative profile.
        if ((peerAddress.S_un.S_addr & 0x00FFFFFF) ==
            (localAddress.S_un.S_addr & 0x00FFFFFF)) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool MasterNet::start(const std::string& bindIp, const std::vector<PeerInit>& inits,
                      uint16_t localPort) {
    if (m_running) {
        return true;
    }
    m_peers.clear();
    if (!m_socket.bind(bindIp.empty() ? "0.0.0.0" : bindIp,
                       localPort != 0 ? localPort : 19881)) {
        return false;
    }
    if (localPort == 0 && !m_socket.open()) {
        return false;
    }

    const uint64_t nowMs = GetTickCount64();
    for (const PeerInit& init : inits) {
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_port = htons(init.port != 0 ? init.port : 19881);
        if (InetPtonA(AF_INET, init.ip.c_str(), &endpoint.sin_addr) != 1) {
            m_socket.close();
            m_peers.clear();
            return false;
        }

        Peer peer{};
        peer.endpoint = endpoint;
        peer.id = init.id.empty() ? init.ip : init.id;
        peer.ip = init.ip;
        peer.filePort = init.port != 0 ? init.port : 19881;
        peer.firstAttemptMs = nowMs;
        m_peers.push_back(std::move(peer));
        resetPeer(m_peers.back(), nowMs);
    }
    m_running = true;
    return true;
}

void MasterNet::stop() {
    std::lock_guard<std::recursive_mutex> lock(m_networkMutex);
    m_running = false;
    m_pendingMouseMove = false;
    m_pendingMoveX = 0;
    m_pendingMoveY = 0;
    m_focusPeerId.clear();
    m_peers.clear();
    m_socket.close();
}

void MasterNet::setPeerId(const std::string& peerId) {
    std::lock_guard<std::recursive_mutex> lock(m_networkMutex);
    m_peerId = peerId.empty() ? "master" : peerId;
}

void MasterNet::reauthenticate() {
    std::lock_guard<std::recursive_mutex> lock(m_networkMutex);
    const uint64_t nowMs = GetTickCount64();
    for (Peer& peer : m_peers) {
        resetPeer(peer, nowMs);
    }
}

void MasterNet::remapPeerIps(const std::vector<PeerInit>& peers) {
    std::lock_guard<std::recursive_mutex> lock(m_networkMutex);
    for (const PeerInit& init : peers) {
        Peer* peer = findPeerByIp(init.ip);
        if (peer != nullptr && !init.id.empty()) {
            peer->id = init.id;
            peer->filePort = init.port != 0 ? init.port : peer->filePort;
        }
    }
}

uint16_t MasterNet::peerFilePortByIp(const std::string& ip) const {
    std::lock_guard<std::recursive_mutex> lock(m_networkMutex);
    for (const Peer& peer : m_peers) {
        if (peer.ip == ip && peer.filePort != 0) {
            return peer.filePort;
        }
    }
    return 19881;
}

MasterNet::Peer* MasterNet::findPeer(const sockaddr_in& endpoint) {
    for (Peer& peer : m_peers) {
        if (sameEndpoint(peer.endpoint, endpoint)) {
            return &peer;
        }
    }
    return nullptr;
}

MasterNet::Peer* MasterNet::findPeerById(const std::string& id) {
    for (Peer& peer : m_peers) {
        if (peer.id == id) {
            return &peer;
        }
    }
    return nullptr;
}

MasterNet::Peer* MasterNet::findPeerByIp(const std::string& ip) {
    for (Peer& peer : m_peers) {
        if (peer.ip == ip) {
            return &peer;
        }
    }
    return nullptr;
}

MasterNet::Peer* MasterNet::firstConnectedPeer() {
    for (Peer& peer : m_peers) {
        if (peer.connected) {
            return &peer;
        }
    }
    return nullptr;
}

MasterNet::Peer* MasterNet::focusedPeer() {
    if (!m_focusPeerId.empty()) {
        Peer* peer = findPeerById(m_focusPeerId);
        if (peer != nullptr && peer->connected) {
            return peer;
        }
        return nullptr;
    }
    return firstConnectedPeer();
}

MasterNet::ScreenSize MasterNet::firstConnectedScreen() const {
    std::lock_guard<std::recursive_mutex> lock(m_networkMutex);
    for (const Peer& peer : m_peers) {
        if (peer.connected && peer.screenW > 0 && peer.screenH > 0) {
            return {true, peer.screenW, peer.screenH};
        }
    }
    return {};
}

MasterNet::ScreenSize MasterNet::peerScreenById(const std::string& id) const {
    std::lock_guard<std::recursive_mutex> lock(m_networkMutex);
    for (const Peer& peer : m_peers) {
        if (peer.id == id) {
            if (peer.connected && peer.screenW > 0 && peer.screenH > 0) {
                return {true, peer.screenW, peer.screenH};
            }
            break;
        }
    }
    return {};
}

bool MasterNet::hasPeer(const std::string& id) const {
    std::lock_guard<std::recursive_mutex> lock(m_networkMutex);
    for (const Peer& peer : m_peers) {
        if (peer.id == id) {
            return true;
        }
    }
    return false;
}

void MasterNet::setFocusedPeer(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lock(m_networkMutex);
    m_focusPeerId = id;
}

void MasterNet::setKcpOverride(const KcpParameters& parameters) {
    std::lock_guard<std::recursive_mutex> lock(m_networkMutex);
    m_kcpOverride = true;
    m_kcpOverrideParameters = parameters;
    for (Peer& peer : m_peers) {
        peer.kcpParameters = parameters;
        if (peer.kcp) {
            peer.kcp->setParameters(parameters);
        }
    }
}

void MasterNet::queueMouseMove(uint16_t x, uint16_t y) {
    std::lock_guard<std::recursive_mutex> lock(m_networkMutex);
    m_pendingMouseMove = true;
    m_pendingMoveX = x;
    m_pendingMoveY = y;
}

bool MasterNet::sendMouseButton(MouseButtonCode button, MouseAction action) {
    std::lock_guard<std::recursive_mutex> lock(m_networkMutex);
    Peer* peer = focusedPeer();
    if (peer == nullptr) {
        return false;
    }
    return sendFrame(*peer, {MessageType::MouseButton, encodeMouseButton(button, action)});
}

bool MasterNet::sendMouseWheel(WheelDirection direction, int16_t delta) {
    std::lock_guard<std::recursive_mutex> lock(m_networkMutex);
    Peer* peer = focusedPeer();
    if (peer == nullptr) {
        return false;
    }
    return sendFrame(*peer, {MessageType::MouseWheel, encodeMouseWheel(direction, delta)});
}

bool MasterNet::sendKeyEvent(bool down, uint16_t vkCode, uint16_t scanCode, bool extendedKey) {
    std::lock_guard<std::recursive_mutex> lock(m_networkMutex);
    Peer* peer = focusedPeer();
    if (peer == nullptr) {
        return false;
    }
    const MessageType type = down ? MessageType::KeyDown : MessageType::KeyUp;
    return sendFrame(*peer, {type, encodeKeyEvent(type, vkCode, scanCode, extendedKey)});
}

bool MasterNet::sendSwitchIn(uint16_t x, uint16_t y) {
    std::lock_guard<std::recursive_mutex> lock(m_networkMutex);
    Peer* peer = focusedPeer();
    if (peer == nullptr) {
        return false;
    }
    m_pendingMouseMove = false;
    if (!sendFrame(*peer, {MessageType::SwitchIn, encodeSwitchIn(x, y)})) {
        return false;
    }
    m_focusPeerId = peer->id;
    return true;
}

bool MasterNet::sendSwitchInTo(const std::string& peerId, uint16_t x, uint16_t y) {
    std::lock_guard<std::recursive_mutex> lock(m_networkMutex);
    Peer* peer = findPeerById(peerId);
    if (peer == nullptr || !peer->connected) {
        return false;
    }
    m_pendingMouseMove = false;
    if (!sendFrame(*peer, {MessageType::SwitchIn, encodeSwitchIn(x, y)})) {
        return false;
    }
    m_focusPeerId = peer->id;
    return true;
}

bool MasterNet::sendSwitchOut() {
    std::lock_guard<std::recursive_mutex> lock(m_networkMutex);
    Peer* peer = focusedPeer();
    if (peer == nullptr) {
        return false;
    }
    m_pendingMouseMove = false;
    return sendFrame(*peer, {MessageType::SwitchOut, {}});
}

bool MasterNet::sendSwitchOutFrom(const std::string& peerId) {
    std::lock_guard<std::recursive_mutex> lock(m_networkMutex);
    Peer* peer = findPeerById(peerId);
    if (peer == nullptr) {
        return false;
    }
    if (!sendFrame(*peer, {MessageType::SwitchOut, {}})) {
        return false;
    }
    if (m_focusPeerId == peerId) {
        m_focusPeerId.clear();
    }
    return true;
}

bool MasterNet::sendClipboardTextTo(const std::string& peerId, const std::string& utf8) {
    std::lock_guard<std::recursive_mutex> lock(m_networkMutex);
    Peer* peer = findPeerById(peerId);
    if (peer == nullptr || !peer->connected || utf8.empty() ||
        utf8.size() > 0xFFFF) {  // 协议帧长度上限 64KB
        return false;
    }
    return sendFrame(*peer, {MessageType::ClipboardText, encodeClipboardText(utf8)});
}

bool MasterNet::sendClipboardRequestTo(const std::string& peerId, bool files) {
    std::lock_guard<std::recursive_mutex> lock(m_networkMutex);
    Peer* peer = findPeerById(peerId);
    if (peer == nullptr || !peer->connected) {
        return false;
    }
    const bool sent = sendFrame(*peer,
                                {MessageType::ClipboardRequest,
                                 encodeClipboardRequest(files)});
    if (sent && peer->kcp) {
        peer->kcp->flush();
    }
    return sent;
}

bool MasterNet::sendFilesMetaTo(const std::string& peerId,
                                const std::vector<FileEntry>& files,
                                const std::string& originPeerId,
                                uint32_t originClipboardSequence) {
    std::lock_guard<std::recursive_mutex> lock(m_networkMutex);
    Peer* peer = findPeerById(peerId);
    if (peer == nullptr || !peer->connected || files.empty()) {
        Logger::write("master: cannot send clipboard files meta peer=" + peerId +
                      ", count=" + std::to_string(files.size()));
        return false;
    }
    const std::vector<uint8_t> payload = encodeFilesMeta(
        files, originPeerId.empty() ? m_peerId : originPeerId,
        originClipboardSequence);
    if (payload.size() > 0xFFFF) {  // protocol frame length is a u16
        Logger::write("master: clipboard files meta too large peer=" + peerId +
                      ", bytes=" + std::to_string(payload.size()));
        return false;
    }
    const bool sent = sendFrame(*peer,
                                {MessageType::ClipboardFilesMeta, std::move(payload)});
    if (sent) {
        peer->kcp->flush();
        Logger::write("master: sent clipboard files meta peer=" + peerId +
                      ", count=" + std::to_string(files.size()) +
                      ", bytes=" + std::to_string(payload.size()));
    } else {
        Logger::write("master: failed to send clipboard files meta peer=" + peerId +
                      ", count=" + std::to_string(files.size()));
    }
    return sent;
}

void MasterNet::resetPeer(Peer& peer, uint64_t nowMs) {
    if (peer.firstAttemptMs == 0) {
        peer.firstAttemptMs = nowMs;
    }
    peer.networkType = sameIpv4Subnet(peer.ip) ? "lan" : "cross";
    peer.kcpParameters = m_kcpOverride
        ? m_kcpOverrideParameters
        : (peer.networkType == "lan" ? kLanKcpParameters
                                     : kCrossNetworkKcpParameters);
    peer.kcp = std::make_unique<KcpReliable>(m_socket, peer.endpoint, kKcpConversation);
    peer.kcp->setParameters(peer.kcpParameters);
    peer.connected = false;
    peer.screenW = 0;
    peer.screenH = 0;
    peer.lastSeenMs = nowMs;
    peer.lastPacketMs = 0;
    peer.lastRawHeartbeatSentMs = 0;
    peer.lastKcpSeenMs = 0;
    peer.lastHandshakeSentMs = nowMs;
    peer.heartbeatSentMs = 0;
    peer.lastRttMs = 0;
    sendHandshake(peer);
}

void MasterNet::sendHandshake(Peer& peer) {
    const std::string key = SecurityKeyStore::load();
    const Message message{
        MessageType::HandshakeRequest,
        encodeHandshakeRequest(m_peerId, key),
    };
    const std::vector<uint8_t> bytes = serializeMessage(message);
    peer.kcp->send(bytes.data(), bytes.size());
}

void MasterNet::sendRawHeartbeat(Peer& peer, uint64_t nowMs) {
    const Message message{
        MessageType::HeartbeatPing,
        encodeHeartbeatPing(m_peerId, sha256Hex(SecurityKeyStore::load())),
    };
    if (sendFrame(peer, message)) {
        peer.lastRawHeartbeatSentMs = nowMs;
    }
}

void MasterNet::applyNetworkProfile(Peer& peer, bool lan) {
    peer.networkType = lan ? "lan" : "cross";
    if (m_kcpOverride) {
        return;
    }
    const KcpParameters desired = lan ? kLanKcpParameters
                                      : kCrossNetworkKcpParameters;
    if (peer.kcpParameters.nodelay == desired.nodelay &&
        peer.kcpParameters.interval == desired.interval &&
        peer.kcpParameters.resend == desired.resend &&
        peer.kcpParameters.nc == desired.nc &&
        peer.kcpParameters.sendWindow == desired.sendWindow &&
        peer.kcpParameters.receiveWindow == desired.receiveWindow) {
        return;
    }
    peer.kcpParameters = desired;
    if (peer.kcp) {
        peer.kcp->setParameters(desired);
    }
    Logger::write("master: peer " + peer.id + " KCP profile changed to " +
                  peer.networkType);
}

void MasterNet::sendHeartbeat(Peer& peer, uint64_t nowMs) {
    const Message message{
        MessageType::Heartbeat,
        encodeHeartbeat(m_peerId),
    };
    const std::vector<uint8_t> bytes = serializeMessage(message);
    peer.kcp->send(bytes.data(), bytes.size());
    peer.lastPacketMs = nowMs;
    peer.heartbeatSentMs = nowMs;
}

void MasterNet::poll(uint64_t nowMs) {
    std::lock_guard<std::recursive_mutex> lock(m_networkMutex);
    if (!m_running) {
        return;
    }

    receiveUdp(nowMs);
    flushPendingMove();

    for (Peer& peer : m_peers) {
        peer.kcp->update(static_cast<IUINT32>(nowMs & 0xFFFFFFFFU));

        if (nowMs - peer.lastRawHeartbeatSentMs >= kRawHeartbeatIntervalMs) {
            sendRawHeartbeat(peer, nowMs);
        }
        if (!peer.connected && peer.endpoint.sin_family != 0 &&
            nowMs - peer.lastHandshakeSentMs >= kHandshakeRetryMs) {
            peer.lastHandshakeSentMs = nowMs;
            sendHandshake(peer);
            if (!peer.udpWarningSent && nowMs - peer.firstAttemptMs >= kUdpWarningMs) {
                peer.udpWarningSent = true;
                Logger::write("master: no UDP handshake response from " + peer.id +
                              "; check firewall, routing, and the preshared key");
            }
        } else if (peer.connected && nowMs - peer.lastPacketMs >= kHeartbeatIntervalMs) {
            sendHeartbeat(peer, nowMs);
        }

        if (peer.connected && nowMs - peer.lastSeenMs >= kPeerTimeoutMs) {
            Logger::write("master: peer " + peer.id + " timed out, reconnecting");
            resetPeer(peer, nowMs);
        } else if (peer.connected && peer.lastKcpSeenMs != 0 &&
                   nowMs - peer.lastKcpSeenMs >= kKcpLivenessMs) {
            Logger::write("master: peer " + peer.id +
                          " raw heartbeat is alive; resetting KCP session");
            resetPeer(peer, nowMs);
        }

        drainKcp(peer, nowMs);
    }
}

void MasterNet::flushPendingMove() {
    std::lock_guard<std::recursive_mutex> lock(m_networkMutex);
    if (!m_pendingMouseMove) {
        return;
    }

    Peer* peer = focusedPeer();
    if (peer == nullptr) {
        m_pendingMouseMove = false;
        return;
    }

    const Message message{
        MessageType::MouseMove,
        encodeMouseMove(peer->mouseSendSequence++, m_pendingMoveX, m_pendingMoveY),
    };
    if (sendFrame(*peer, message)) {
        m_pendingMouseMove = false;
    }
}

void MasterNet::receiveUdp(uint64_t nowMs) {
    std::vector<uint8_t> buffer(65536);
    for (;;) {
        sockaddr_in source{};
        const int size = m_socket.recvFrom(source, buffer.data(), buffer.size());
        if (size <= 0) {
            return;
        }

        if (size < 4) {
            continue;
        }

        if (readU16(buffer.data()) != kKcpPacketMagic) {
            size_t offset = 0;
            Message message{MessageType::Heartbeat, {}};
            while (decodeMessage(buffer.data(), static_cast<size_t>(size), offset, message)) {
                if (message.type == MessageType::HeartbeatPong) {
                    std::string peerId;
                    if (decodeHeartbeat(message.payload, peerId)) {
                        Peer* heartbeatPeer = findPeer(source);
                        if (heartbeatPeer != nullptr) {
                            heartbeatPeer->lastSeenMs = nowMs;
                        }
                    }
                    continue;
                }
                if (message.type != MessageType::SlaveHello) {
                    continue;
                }
                std::string hostname;
                uint16_t filePort = 19881;
                std::string presharedKey;
                if (!decodeSlaveHello(message.payload, hostname, filePort, presharedKey) ||
                    presharedKey != SecurityKeyStore::load()) {
                    Logger::write("master: slave hello rejected from " +
                                  endpointToString(source) +
                                  ": key mismatch or malformed request");
                    continue;
                }

                const std::string sourceIp = endpointAddressToString(source);
                Peer* peer = findPeerByIp(sourceIp);
                if (peer == nullptr) {
                    Peer dynamicPeer{};
                    dynamicPeer.id = hostname;
                    dynamicPeer.ip = sourceIp;
                    m_peers.push_back(std::move(dynamicPeer));
                    peer = &m_peers.back();
                    Logger::write("master: unknown slave online from " + sourceIp +
                                  " (" + hostname + ")");
                }
                peer->endpoint = source;
                peer->filePort = filePort != 0 ? filePort : 19881;
                peer->firstAttemptMs = nowMs;
                resetPeer(*peer, nowMs);
            }
            continue;
        }

        Peer* peer = findPeer(source);
        if (peer == nullptr) {
            // A KCP packet from an unknown source would bypass hello/key auth.
            continue;
        }
        peer->kcp->input(reinterpret_cast<char*>(buffer.data() + 2), size - 2);
        peer->kcp->update(static_cast<IUINT32>(nowMs & 0xFFFFFFFFU));
        drainKcp(*peer, nowMs);
    }
}

void MasterNet::drainKcp(Peer& peer, uint64_t nowMs) {
    std::vector<uint8_t> buffer(65536);
    for (;;) {
        const int size = peer.kcp->recv(buffer.data(), buffer.size());
        if (size <= 0) {
            return;
        }

        size_t offset = 0;
        Message message{MessageType::Heartbeat, {}};
        while (decodeMessage(buffer.data(), static_cast<size_t>(size), offset, message)) {
            handleFrame(peer, message, nowMs);
        }
    }
}

void MasterNet::handleFrame(Peer& peer, const Message& message, uint64_t nowMs) {
    switch (message.type) {
        case MessageType::HandshakeResponse: {
            std::string peerId;
            if (decodeHandshakeResponse(message.payload, peerId, peer.screenW, peer.screenH)) {
                peer.hostname = peerId;
                if (!peer.connected) {
                    peer.connected = true;
                    peer.lastRttMs = 0;
                    peer.lastKcpSeenMs = nowMs;
                    Logger::write("master: peer " + peer.id + " connected");
                }
                peer.lastSeenMs = nowMs;
                peer.lastPacketMs = nowMs;
            }
            break;
        }
        case MessageType::Heartbeat: {
            std::string peerId;
            if (decodeHeartbeat(message.payload, peerId)) {
                peer.lastSeenMs = nowMs;
                peer.lastKcpSeenMs = nowMs;
                // The slave replies to every heartbeat, so this delta is the RTT.
                if (peer.heartbeatSentMs != 0) {
                    peer.lastRttMs = static_cast<uint32_t>(nowMs - peer.heartbeatSentMs);
                    // RTT below 2ms is the LAN profile boundary requested in P8.
                    applyNetworkProfile(peer, peer.lastRttMs < 2);
                }
            }
            break;
        }
        case MessageType::ClipboardText: {
            std::string utf8;
            if (decodeClipboardText(message.payload, utf8) && !utf8.empty() &&
                onClipboardText) {
                onClipboardText(peer.id, utf8);
            }
            break;
        }
        case MessageType::ClipboardFilesMeta: {
            std::vector<FileEntry> files;
            std::string originPeerId;
            uint32_t originClipboardSequence = 0;
            if (decodeFilesMeta(message.payload, files, originPeerId,
                                originClipboardSequence) && !files.empty()) {
                if (onClipboardFiles) {
                    Logger::write("master: received clipboard files meta peer=" +
                                  peer.id + ", count=" + std::to_string(files.size()));
                    onClipboardFiles(peer.id, originPeerId, originClipboardSequence,
                                     files);
                }
            } else {
                Logger::write("master: received invalid clipboard files meta peer=" +
                              peer.id + ", bytes=" +
                              std::to_string(message.payload.size()));
            }
            break;
        }
        case MessageType::ClipboardRequest: {
            bool files = false;
            if (decodeClipboardRequest(message.payload, files) && files &&
                onClipboardFilesRequest) {
                Logger::write("master: received clipboard files request peer=" + peer.id);
                onClipboardFilesRequest(peer.id, peer.ip);
            }
            break;
        }
        default:
            break;
    }
}

bool MasterNet::sendFrame(Peer& peer, const Message& message) {
    std::lock_guard<std::recursive_mutex> lock(m_networkMutex);
    const std::vector<uint8_t> bytes = serializeMessage(message);
    if (routeChannel(message.type) == Channel::UdpRaw) {
        return m_socket.sendTo(peer.endpoint, bytes.data(), bytes.size());
    }
    return peer.kcp != nullptr && peer.kcp->send(bytes.data(), bytes.size());
}

}  // namespace synccontrol
