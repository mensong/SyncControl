#include "net/SlaveNet.h"

#include <Windows.h>

#include <algorithm>
#include <fstream>
#include <json.hpp>

#include "core/Logger.h"
#include "core/Security.h"
#include "net/Protocol.h"

namespace synccontrol {
namespace {

constexpr uint16_t kKcpConversation = 1;
constexpr uint64_t kPeerTimeoutMs = 8000;
constexpr uint8_t kKcpCmdPush = 81;
constexpr size_t kKcpSegmentHeaderSize = 24;

bool sameEndpoint(const sockaddr_in& left, const sockaddr_in& right) {
    return left.sin_family == right.sin_family &&
           left.sin_addr.s_addr == right.sin_addr.s_addr &&
           left.sin_port == right.sin_port;
}

uint16_t readU16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8);
}

uint32_t readU32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           static_cast<uint32_t>(data[1]) << 8 |
           static_cast<uint32_t>(data[2]) << 16 |
           static_cast<uint32_t>(data[3]) << 24;
}

std::string localHostname() {
    char hostName[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD length = sizeof(hostName);
    if (!GetComputerNameA(hostName, &length)) {
        return "slave";
    }
    return hostName;
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
        if ((peerAddress.S_un.S_addr & 0x00FFFFFF) ==
            (localAddress.S_un.S_addr & 0x00FFFFFF)) {
            return true;
        }
    }
    return false;
}

bool hasLocalFiles(const std::vector<FileEntry>& files) {
    return !files.empty() && std::any_of(files.begin(), files.end(),
                                         [](const FileEntry& entry) {
                                             return !entry.path.empty();
                                         });
}

// A restarted master opens a brand-new KCP stream with sequence numbers
// reset to zero. A stale local session would drop those segments forever,
// so recognize an authenticated handshake request from the raw datagram and
// report its sequence before ikcp_input discards it.
bool isAuthenticatedHandshakePacket(const std::vector<uint8_t>& buffer, size_t size,
                                    uint32_t& sequence) {
    if (size < 2 + kKcpSegmentHeaderSize) {
        return false;
    }
    const uint8_t* segment = buffer.data() + 2;
    if (segment[4] != kKcpCmdPush) {
        return false;
    }
    sequence = readU32(segment + 12);
    const uint32_t length = readU32(segment + 20);
    if (length == 0 || size < 2 + kKcpSegmentHeaderSize + length) {
        return false;
    }
    size_t offset = 0;
    Message message{MessageType::Heartbeat, {}};
    if (!decodeMessage(segment + kKcpSegmentHeaderSize, length, offset, message) ||
        message.type != MessageType::HandshakeRequest) {
        return false;
    }
    std::string peerId;
    std::string keyHash;
    return decodeHandshakeRequest(message.payload, peerId, keyHash) &&
           keyHash == sha256Hex(SecurityKeyStore::load());
}

}  // namespace

bool SlaveNet::start(uint16_t localPort, const std::string& masterIp, uint16_t masterPort) {
    std::lock_guard<std::recursive_mutex> lock(m_networkMutex);
    if (m_running) {
        return true;
    }

    m_peerId = localHostname();
    m_masterIp = masterIp;
    m_masterPort = masterPort;
    m_masterEndpoint.sin_family = AF_INET;
    m_masterEndpoint.sin_port = htons(masterPort);
    if (InetPtonA(AF_INET, masterIp.c_str(), &m_masterEndpoint.sin_addr) != 1) {
        Logger::write("slave: invalid master IP " + masterIp);
        m_masterEndpoint = {};
        m_running = m_socket.bind(localPort);
        return m_running;
    }
    m_running = m_socket.bind(localPort);
    return m_running;
}

void SlaveNet::setKcpOverride(const KcpParameters& parameters) {
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

void SlaveNet::disconnectAll() {
    std::lock_guard<std::recursive_mutex> lock(m_networkMutex);
    if (!m_peers.empty()) {
        Logger::write("slave: disconnected all masters for key re-authentication");
    }
    m_peers.clear();
    if (m_inputActive) {
        m_inputActive = false;
        m_input.switchOut();
    }
}

SlaveNet::MouseStats SlaveNet::mouseStats() const {
    std::lock_guard<std::recursive_mutex> lock(m_networkMutex);
    uint64_t received = 0;
    uint64_t lost = 0;
    for (const Peer& peer : m_peers) {
        received += peer.mouseReceiveCount;
        lost += peer.mouseLossCount;
    }
    return {received, lost};
}

void SlaveNet::stop() {
    std::lock_guard<std::recursive_mutex> lock(m_networkMutex);
    m_running = false;
    m_inputActive = false;
    m_peers.clear();
    m_clipboardImages.clear();
    m_clipboardRich.clear();
    m_socket.close();
}

std::string nextClipboardImageId() {
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

std::string nextClipboardRichId() {
    static uint64_t counter = 0;
    return "syncctl-rich-" + std::to_string(GetTickCount64()) + "-" +
           std::to_string(++counter);
}

std::string richExtension(ClipboardRichKind kind) {
    return kind == ClipboardRichKind::Rtf ? ".rtf" : ".html";
}

bool clipboardRichDirectory(std::string& directory) {
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
    directory = base + "\\clipboard\\rich";
    if (!CreateDirectoryA(directory.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    return true;
}

bool writeClipboardRichFile(const std::string& path,
                            const std::vector<uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    if (!bytes.empty()) {
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    return static_cast<bool>(file);
}

SlaveNet::Peer* SlaveNet::findPeer(const sockaddr_in& endpoint) {
    for (Peer& peer : m_peers) {
        if (sameEndpoint(peer.endpoint, endpoint)) {
            return &peer;
        }
    }
    return nullptr;
}

SlaveNet::Peer& SlaveNet::ensurePeer(const sockaddr_in& endpoint, uint64_t nowMs) {
    if (Peer* existing = findPeer(endpoint)) {
        return *existing;
    }

    Peer peer{};
    peer.endpoint = endpoint;
    peer.kcp = std::make_unique<KcpReliable>(m_socket, endpoint, kKcpConversation);
    peer.lastSeenMs = nowMs;
    peer.connectedSinceMs = nowMs;
    m_peers.push_back(std::move(peer));
    return m_peers.back();
}

void SlaveNet::removeTimedOutPeers(uint64_t nowMs) {
    for (const Peer& peer : m_peers) {
        if (nowMs - peer.lastSeenMs >= kPeerTimeoutMs) {
            Logger::write("slave: master " + peer.id + " timed out, waiting for reconnect");
        }
    }
    m_peers.erase(
        std::remove_if(m_peers.begin(), m_peers.end(),
                       [nowMs](const Peer& peer) {
                           return nowMs - peer.lastSeenMs >= kPeerTimeoutMs ||
                                  peer.authRejected;
                       }),
        m_peers.end());
    if (m_peers.empty()) {
        if (m_inputActive) {
            m_inputActive = false;
            Logger::write("slave: input detached, all masters disconnected");
        }
    }
}

void SlaveNet::poll(uint64_t nowMs) {
    std::lock_guard<std::recursive_mutex> lock(m_networkMutex);
    if (!m_running) {
        return;
    }

    receiveUdp(nowMs);
    if (m_peers.empty() && nowMs - m_lastHelloMs >= 3000) {
        sendSlaveHello();
        m_lastHelloMs = nowMs;
    }
    for (Peer& peer : m_peers) {
        peer.kcp->update(static_cast<IUINT32>(nowMs & 0xFFFFFFFFU));
        drainKcp(peer, nowMs);
    }
    removeTimedOutPeers(nowMs);
}

void SlaveNet::receiveUdp(uint64_t nowMs) {
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
            Message message{MessageType::MouseMove, {}};
            // Raw input is not authenticated at the packet level; reject unknown sources.
            Peer* peer = findPeer(source);
            if (peer == nullptr || peer->authRejected) {
                continue;
            }
            while (decodeMessage(buffer.data(), static_cast<size_t>(size), offset, message)) {
                if (message.type == MessageType::HeartbeatPing) {
                    std::string peerId;
                    std::string keyHash;
                    if (decodeHeartbeatPing(message.payload, peerId, keyHash) &&
                        keyHash == sha256Hex(SecurityKeyStore::load())) {
                        peer->lastSeenMs = nowMs;
                        const Message pong{
                            MessageType::HeartbeatPong,
                            encodeHeartbeat(peer->id.empty() ? peerId : peer->id),
                        };
                        const std::vector<uint8_t> bytes = serializeMessage(pong);
                        m_socket.sendTo(peer->endpoint, bytes.data(), bytes.size());
                    }
                    continue;
                }
                if (message.type == MessageType::HandshakeResponse) {
                    std::string peerId;
                    uint16_t width = 0;
                    uint16_t height = 0;
                    if (decodeHandshakeResponse(message.payload, peerId, width, height)) {
                        m_lastHelloMs = nowMs;
                    }
                } else if (message.type == MessageType::MouseMove) {
                    handleMouseMove(peer, message);
                }
            }
            continue;
        }

        Peer& peer = ensurePeer(source, nowMs);
        uint32_t incomingSequence = 0;
        if (isAuthenticatedHandshakePacket(buffer, static_cast<size_t>(size),
                                           incomingSequence) &&
            incomingSequence < peer.kcp->rcvNext()) {
            peer.lastSeenMs = nowMs;
            peer.kcp->resetSession();
        }
        peer.kcp->input(reinterpret_cast<char*>(buffer.data() + 2), size - 2);
        peer.kcp->update(static_cast<IUINT32>(nowMs & 0xFFFFFFFFU));
        drainKcp(peer, nowMs);
    }

}

void SlaveNet::drainKcp(Peer& peer, uint64_t nowMs) {
    std::vector<uint8_t> buffer(65536);
    for (;;) {
        const int size = peer.kcp->recv(buffer.data(), buffer.size());
        if (size <= 0) {
            return;
        }

        size_t offset = 0;
        Message message{MessageType::Heartbeat, {}};
        while (decodeMessage(buffer.data(), static_cast<size_t>(size), offset, message)) {
            peer.lastSeenMs = nowMs;
            handleFrame(peer, message, nowMs);
        }
    }
}

void SlaveNet::handleFrame(Peer& peer, const Message& message, uint64_t nowMs) {
    switch (message.type) {
        case MessageType::HandshakeRequest: {
            std::string peerId;
            std::string keyHash;
            const std::string expectedKeyHash =
                sha256Hex(SecurityKeyStore::load());
            if (decodeHandshakeRequest(message.payload, peerId, keyHash) &&
                keyHash == expectedKeyHash) {
                peer.id = peerId;
                peer.lastSeenMs = nowMs;
                peer.authRejected = false;
                peer.authFailureLogged = false;
                peer.kcpParameters = m_kcpOverride
                    ? m_kcpOverrideParameters
                    : (sameIpv4Subnet(endpointAddressToString(peer.endpoint))
                           ? kLanKcpParameters
                           : kCrossNetworkKcpParameters);
                if (peer.kcp) {
                    peer.kcp->setParameters(peer.kcpParameters);
                }
                sendHandshakeResponse(peer);
            } else if (!peer.authFailureLogged) {
                peer.authFailureLogged = true;
                peer.authRejected = true;
                Logger::write("slave: handshake rejected for " + peerId +
                              ": preshared key mismatch or malformed request");
            }
            break;
        }
        case MessageType::Heartbeat: {
            std::string peerId;
            if (decodeHeartbeat(message.payload, peerId)) {
                peer.id = peerId.empty() ? peer.id : peerId;
                peer.lastSeenMs = nowMs;
                sendHeartbeat(peer);
            }
            break;
        }
        case MessageType::ClipboardText: {
            std::string utf8;
            if (decodeClipboardText(message.payload, utf8) && !utf8.empty() &&
                m_clipboard != nullptr) {
                m_clipboard->setTextToClipboard(utf8);
            }
            break;
        }
        case MessageType::ClipboardImageMeta: {
            ClipboardImageMeta meta;
            if (decodeClipboardImageMeta(message.payload, meta)) {
                handleClipboardImageMeta(meta);
            } else {
                Logger::write("slave: received invalid clipboard image meta, bytes=" +
                              std::to_string(message.payload.size()));
            }
            break;
        }
        case MessageType::ClipboardRichMeta: {
            ClipboardRichMeta meta;
            if (decodeClipboardRichMeta(message.payload, meta)) {
                handleClipboardRichMeta(meta);
            } else {
                Logger::write("slave: received invalid clipboard rich meta, bytes=" +
                              std::to_string(message.payload.size()));
            }
            break;
        }
        case MessageType::ClipboardRequest: {
            bool files = false;
            if (!decodeClipboardRequest(message.payload, files)) {
                break;
            }
            if (m_clipboard == nullptr) {
                break;
            }
            if (files) {
                if (m_fileTransfer != nullptr) {
                    m_clipboard->refresh();
                    const ClipboardMonitor::Snapshot snapshot = m_clipboard->snapshot();
                    if (hasLocalFiles(snapshot.files)) {
                    m_fileTransfer->sendAsync(snapshot.files,
                                              endpointAddressToString(peer.endpoint),
                                              m_masterPort);
                    }
                }
                break;
            }
            {
                m_clipboard->refresh();
                const ClipboardMonitor::Snapshot snapshot = m_clipboard->snapshot();
                if (hasLocalFiles(snapshot.files)) {
                    const bool hasOrigin = !snapshot.originPeerId.empty();
                    sendFilesMeta(peer, snapshot.files,
                                  hasOrigin ? snapshot.originPeerId : m_peerId,
                                  hasOrigin && snapshot.originClipboardSequence != 0
                                      ? snapshot.originClipboardSequence
                                      : snapshot.clipboardSequence);
                } else if (snapshot.state == ClipboardMonitor::State::Text &&
                           !snapshot.text.empty()) {
                    sendKcpFrame(peer, {MessageType::ClipboardText,
                                        encodeClipboardText(snapshot.text)});
                }
                if (snapshot.state == ClipboardMonitor::State::Image &&
                    !snapshot.pngData.empty()) {
                    sendClipboardImageMeta(peer, snapshot);
                } else if ((snapshot.state == ClipboardMonitor::State::Html &&
                            !snapshot.htmlData.empty()) ||
                           (snapshot.state == ClipboardMonitor::State::Rtf &&
                            !snapshot.rtfData.empty())) {
                    sendClipboardRichMeta(peer, snapshot);
                }
            }
            break;
        }
        case MessageType::ClipboardImageRequest: {
            std::string transferId;
            if (decodeClipboardImageRequest(message.payload, transferId)) {
                serveClipboardImageRequest(peer, transferId);
            }
            break;
        }
        case MessageType::ClipboardRichRequest: {
            std::string transferId;
            if (decodeClipboardRichRequest(message.payload, transferId)) {
                serveClipboardRichRequest(peer, transferId);
            }
            break;
        }
        case MessageType::ClipboardFilesMeta: {
            std::vector<FileEntry> files;
            std::string originPeerId;
            uint32_t originClipboardSequence = 0;
            if (decodeFilesMeta(message.payload, files, originPeerId,
                                originClipboardSequence) && !files.empty()) {
                if (m_remoteFilesMeta) {
                    m_remoteFilesMeta(originPeerId, originClipboardSequence, files);
                } else if (m_clipboard != nullptr) {
                    const bool installed = m_clipboard->setPendingFilesMeta(files);
                    Logger::write(std::string("slave: ") +
                                  (installed ? "installed" : "failed to install") +
                                  " clipboard files meta, count=" +
                                  std::to_string(files.size()) + ", bytes=" +
                                  std::to_string(message.payload.size()));
                } else {
                    Logger::write("slave: clipboard monitor unavailable for files meta");
                }
            } else {
                Logger::write("slave: received invalid clipboard files meta, bytes=" +
                              std::to_string(message.payload.size()));
            }
            break;
        }
        default:
            handleInputMessage(message);
            break;
    }
}

bool SlaveNet::sendKcpFrame(Peer& peer, const Message& message) {
    if (peer.kcp == nullptr) {
        return false;
    }
    const std::vector<uint8_t> bytes = serializeMessage(message);
    return peer.kcp->send(bytes.data(), bytes.size());
}

void SlaveNet::sendFilesMeta(Peer& peer, const std::vector<FileEntry>& files,
                             const std::string& originPeerId,
                             uint32_t originClipboardSequence) {
    const std::vector<uint8_t> payload =
        encodeFilesMeta(files, originPeerId, originClipboardSequence);
    if (payload.size() > 0xFFFF) {
        Logger::write("slave: clipboard files meta too large, bytes=" +
                      std::to_string(payload.size()));
        return;
    }
    const bool sent = sendKcpFrame(peer,
                                   {MessageType::ClipboardFilesMeta,
                                    std::move(payload)});
    if (sent) {
        peer.kcp->flush();
        Logger::write("slave: sent clipboard files meta peer=" + peer.id +
                      ", count=" + std::to_string(files.size()) +
                      ", bytes=" + std::to_string(payload.size()));
    } else {
        Logger::write("slave: failed to send clipboard files meta peer=" + peer.id);
    }
}

void SlaveNet::handleClipboardImageMeta(const ClipboardImageMeta& meta) {
    if (m_remoteImageMeta) {
        Logger::write("slave: received clipboard image meta, transferId=" +
                      meta.transferId + ", bytes=" +
                      std::to_string(meta.totalBytes));
        m_remoteImageMeta(meta);
        return;
    }
    Logger::write("slave: clipboard image meta has no handler, transferId=" +
                  meta.transferId);
}

void SlaveNet::handleClipboardRichMeta(const ClipboardRichMeta& meta) {
    if (m_remoteRichMeta) {
        Logger::write("slave: received clipboard rich meta, transferId=" +
                      meta.transferId + ", bytes=" +
                      std::to_string(meta.richBytes));
        m_remoteRichMeta(meta);
        return;
    }
    Logger::write("slave: clipboard rich meta has no handler, transferId=" +
                  meta.transferId);
}

void SlaveNet::sendClipboardImageMeta(
    Peer& peer, const ClipboardMonitor::Snapshot& snapshot) {
    if (snapshot.pngData.empty() ||
        snapshot.pngData.size() > kMaxClipboardImageBytes) {
        Logger::write("slave: clipboard image rejected, bytes=" +
                      std::to_string(snapshot.pngData.size()) +
                      ", limit=" + std::to_string(kMaxClipboardImageBytes));
        return;
    }

    ClipboardImageMeta meta;
    meta.transferId = nextClipboardImageId();
    meta.width = snapshot.imageWidth;
    meta.height = snapshot.imageHeight;
    meta.totalBytes = static_cast<uint32_t>(snapshot.pngData.size());
    meta.originPeerId = snapshot.originPeerId.empty()
                            ? m_peerId
                            : snapshot.originPeerId;
    meta.originClipboardSequence =
        snapshot.originPeerId.empty()
            ? snapshot.clipboardSequence
            : snapshot.originClipboardSequence;
    if (!saveClipboardImageFile(meta.transferId, snapshot.pngData,
                                m_clipboardImages[meta.transferId].path)) {
        m_clipboardImages.erase(meta.transferId);
        Logger::write("slave: failed to save clipboard image cache, transferId=" +
                      meta.transferId);
        return;
    }
    m_clipboardImages[meta.transferId].width = meta.width;
    m_clipboardImages[meta.transferId].height = meta.height;
    m_clipboardImages[meta.transferId].bytes = meta.totalBytes;

    const std::vector<uint8_t> payload = encodeClipboardImageMeta(meta);
    if (sendKcpFrame(peer, {MessageType::ClipboardImageMeta,
                            std::move(payload)})) {
        peer.kcp->flush();
        Logger::write("slave: sent clipboard image meta peer=" + peer.id +
                      ", transferId=" + meta.transferId +
                      ", bytes=" + std::to_string(meta.totalBytes));
    }
}

void SlaveNet::sendClipboardRichMeta(
    Peer& peer, const ClipboardMonitor::Snapshot& snapshot) {
    const std::vector<uint8_t>& richData =
        snapshot.state == ClipboardMonitor::State::Rtf
            ? snapshot.rtfData
            : snapshot.htmlData;
    if (richData.empty() || richData.size() > kMaxClipboardRichBytes) {
        Logger::write("slave: clipboard rich content rejected, bytes=" +
                      std::to_string(richData.size()) +
                      ", limit=" + std::to_string(kMaxClipboardRichBytes));
        return;
    }

    std::string directory;
    if (!clipboardRichDirectory(directory)) {
        Logger::write("slave: cannot resolve clipboard rich cache directory");
        return;
    }

    ClipboardRichMeta meta;
    meta.transferId = nextClipboardRichId();
    meta.kind = snapshot.state == ClipboardMonitor::State::Rtf
                    ? ClipboardRichKind::Rtf
                    : ClipboardRichKind::Html;
    meta.richBytes = richData.size();
    meta.companionTextBytes = snapshot.text.size();
    meta.originPeerId = snapshot.originPeerId.empty()
                            ? m_peerId
                            : snapshot.originPeerId;
    meta.originClipboardSequence =
        snapshot.originPeerId.empty()
            ? snapshot.clipboardSequence
            : snapshot.originClipboardSequence;

    CachedRich& cache = m_clipboardRich[meta.transferId];
    cache.kind = meta.kind;
    cache.richPath = directory + "\\" + meta.transferId +
                     richExtension(meta.kind);
    cache.textPath = directory + "\\" + meta.transferId + ".txt";
    cache.richBytes = meta.richBytes;
    cache.textBytes = meta.companionTextBytes;
    const std::vector<uint8_t> textData(snapshot.text.begin(),
                                        snapshot.text.end());
    if (!writeClipboardRichFile(cache.richPath, richData) ||
        !writeClipboardRichFile(cache.textPath, textData)) {
        m_clipboardRich.erase(meta.transferId);
        Logger::write("slave: failed to save clipboard rich cache, transferId=" +
                      meta.transferId);
        return;
    }

    const std::vector<uint8_t> payload = encodeClipboardRichMeta(meta);
    if (sendKcpFrame(peer, {MessageType::ClipboardRichMeta,
                            std::move(payload)})) {
        peer.kcp->flush();
        Logger::write("slave: sent clipboard rich meta peer=" + peer.id +
                      ", transferId=" + meta.transferId +
                      ", bytes=" + std::to_string(meta.richBytes));
    }
}

void SlaveNet::serveClipboardImageRequest(Peer& peer,
                                          const std::string& transferId) {
    if (m_fileTransfer == nullptr) {
        Logger::write("slave: cannot serve clipboard image, no file transfer");
        return;
    }
    auto image = m_clipboardImages.find(transferId);
    if (image == m_clipboardImages.end()) {
        Logger::write("slave: unknown clipboard image request, transferId=" +
                      transferId);
        return;
    }

    FileEntry entry;
    entry.path = image->second.path;
    entry.name = transferId + ".png";
    entry.size = image->second.bytes;
    Logger::write("slave: sending clipboard image peer=" + peer.id +
                  ", transferId=" + transferId + ", bytes=" +
                  std::to_string(entry.size));
    m_fileTransfer->sendAsync({entry}, endpointAddressToString(peer.endpoint),
                              m_masterPort);
}

void SlaveNet::serveClipboardRichRequest(Peer& peer,
                                         const std::string& transferId) {
    if (m_fileTransfer == nullptr) {
        Logger::write("slave: cannot serve clipboard rich content, no file transfer");
        return;
    }

    auto rich = m_clipboardRich.find(transferId);
    if (rich == m_clipboardRich.end()) {
        Logger::write("slave: unknown clipboard rich request, transferId=" +
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
    Logger::write("slave: sending clipboard rich content peer=" + peer.id +
                  ", transferId=" + transferId +
                  ", bytes=" + std::to_string(richEntry.size + textEntry.size));
    m_fileTransfer->sendAsync({richEntry, textEntry},
                              endpointAddressToString(peer.endpoint),
                              m_masterPort);
}

std::vector<FileEntry> SlaveNet::requestClipboardFiles(
    const std::atomic<bool>& cancel) {
    if (cancel.load()) {
        return {};
    }
    if (!m_running || m_fileTransfer == nullptr || m_peers.empty()) {
        Logger::write("slave: cannot request clipboard files, running=" +
                      std::to_string(m_running ? 1 : 0) +
                      ", fileTransfer=" +
                      std::to_string(m_fileTransfer != nullptr ? 1 : 0) +
                      ", peers=" + std::to_string(m_peers.size()));
        return {};
    }

    const uint64_t token = m_fileTransfer->beginReceiveWait();
    {
        std::lock_guard<std::recursive_mutex> lock(m_networkMutex);
        if (m_peers.empty()) {
            Logger::write("slave: cannot request clipboard files, no connected master");
            return {};
        }
        Peer& peer = m_peers.front();
        const bool sent = sendKcpFrame(peer,
                                       {MessageType::ClipboardRequest,
                                        encodeClipboardRequest(true)});
        if (sent && peer.kcp) {
            peer.kcp->flush();
        }
        Logger::write("slave: requested clipboard files from master, sent=" +
                      std::to_string(sent ? 1 : 0));
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
                      ? "slave: clipboard files request cancelled"
                      : "slave: timed out waiting for clipboard files");
    return {};
}

std::vector<FileEntry> SlaveNet::requestClipboardImage(
    const std::string& transferId, const std::atomic<bool>& cancel) {
    if (cancel.load() || transferId.empty()) {
        return {};
    }
    if (!m_running || m_fileTransfer == nullptr || m_peers.empty()) {
        Logger::write("slave: cannot request clipboard image, running=" +
                      std::to_string(m_running ? 1 : 0) +
                      ", fileTransfer=" +
                      std::to_string(m_fileTransfer != nullptr ? 1 : 0) +
                      ", peers=" + std::to_string(m_peers.size()));
        return {};
    }

    const uint64_t token = m_fileTransfer->beginReceiveWait();
    bool sent = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_networkMutex);
        if (!m_peers.empty()) {
            Peer& peer = m_peers.front();
            sent = sendKcpFrame(peer,
                                {MessageType::ClipboardImageRequest,
                                 encodeClipboardImageRequest(transferId)});
            if (sent && peer.kcp) {
                peer.kcp->flush();
            }
        }
    }
    if (!sent) {
        Logger::write("slave: failed to send clipboard image request, transferId=" +
                      transferId);
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
                      ? "slave: clipboard image request cancelled"
                      : "slave: timed out waiting for clipboard image");
    return {};
}

std::vector<FileEntry> SlaveNet::requestClipboardRich(
    const std::string& transferId, const std::atomic<bool>& cancel) {
    if (cancel.load() || transferId.empty()) {
        return {};
    }
    if (!m_running || m_fileTransfer == nullptr || m_peers.empty()) {
        Logger::write("slave: cannot request clipboard rich content, running=" +
                      std::to_string(m_running ? 1 : 0) +
                      ", fileTransfer=" +
                      std::to_string(m_fileTransfer != nullptr ? 1 : 0) +
                      ", peers=" + std::to_string(m_peers.size()));
        return {};
    }

    const uint64_t token = m_fileTransfer->beginReceiveWait();
    bool sent = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_networkMutex);
        if (!m_peers.empty()) {
            Peer& peer = m_peers.front();
            sent = sendKcpFrame(peer,
                                {MessageType::ClipboardRichRequest,
                                 encodeClipboardRichRequest(transferId)});
            if (sent && peer.kcp) {
                peer.kcp->flush();
            }
        }
    }
    if (!sent) {
        Logger::write("slave: failed to send clipboard rich request, transferId=" +
                      transferId);
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
                      ? "slave: clipboard rich request cancelled"
                      : "slave: timed out waiting for clipboard rich content");
    return {};
}

void SlaveNet::handleInputMessage(const Message& message) {
    const auto inject = [this](const char* what, bool ok) {
        if (!ok) {
            Logger::write(std::string("slave: inject failed: ") + what);
        }
    };
    switch (message.type) {
        case MessageType::MouseButton: {
            MouseButtonCode button = MouseButtonCode::Left;
            MouseAction action = MouseAction::Down;
            if (m_inputActive && decodeMouseButton(message.payload, button, action)) {
                inject("mouse button", m_input.mouseButton(button, action));
            }
            break;
        }
        case MessageType::MouseWheel: {
            WheelDirection direction = WheelDirection::Vertical;
            int16_t delta = 0;
            if (m_inputActive && decodeMouseWheel(message.payload, direction, delta)) {
                inject("mouse wheel", m_input.mouseWheel(direction, delta));
            }
            break;
        }
        case MessageType::KeyDown:
        case MessageType::KeyUp: {
            uint16_t vkCode = 0;
            uint16_t scanCode = 0;
            bool extendedKey = false;
            if (m_inputActive && decodeKeyEvent(message.payload, vkCode, scanCode, extendedKey)) {
                inject("key", m_input.keyEvent(message.type == MessageType::KeyDown, vkCode,
                                               scanCode, extendedKey));
            }
            break;
        }
        case MessageType::SwitchIn: {
            uint16_t x = 0;
            uint16_t y = 0;
            if (decodeSwitchIn(message.payload, x, y) && m_input.switchIn(x, y)) {
                m_inputActive = true;
            }
            break;
        }
        case MessageType::SwitchOut:
            m_inputActive = false;
            m_input.switchOut();
            break;
        default:
            break;
    }
}

void SlaveNet::handleMouseMove(Peer* peer, const Message& message) {
    if (peer == nullptr) {
        return;
    }
    uint32_t sequence = 0;
    uint16_t x = 0;
    uint16_t y = 0;
    if (!decodeMouseMove(message.payload, sequence, x, y)) {
        return;
    }

    // Raw UDP has no ACK. A sequence jump is a lost datagram, not a
    // retransmission candidate; stale packets are ignored for cursor smoothness.
    if (!peer->hasMouseSequence) {
        peer->hasMouseSequence = true;
        peer->expectedMouseSequence = sequence;
    } else if (sequence > peer->expectedMouseSequence) {
        peer->mouseLossCount += sequence - peer->expectedMouseSequence;
        peer->expectedMouseSequence = sequence;
    } else if (sequence < peer->expectedMouseSequence) {
        return;
    }
    peer->expectedMouseSequence = sequence + 1;
    peer->mouseReceiveCount++;

    if (!m_inputActive) {
        return;
    }
    if (!m_input.mouseMove(x, y)) {
        Logger::write("slave: inject failed: mouse move");
    }
}

void SlaveNet::sendHandshakeResponse(Peer& peer) {
    const Message message{
        MessageType::HandshakeResponse,
        encodeHandshakeResponse(m_peerId,
                                static_cast<uint16_t>(GetSystemMetrics(SM_CXSCREEN)),
                                static_cast<uint16_t>(GetSystemMetrics(SM_CYSCREEN))),
    };
    const std::vector<uint8_t> bytes = serializeMessage(message);
    peer.kcp->send(bytes.data(), bytes.size());
}

void SlaveNet::sendHeartbeat(Peer& peer) {
    const Message message{
        MessageType::Heartbeat,
        encodeHeartbeat(m_peerId),
    };
    const std::vector<uint8_t> bytes = serializeMessage(message);
    peer.kcp->send(bytes.data(), bytes.size());
}

void SlaveNet::sendSlaveHello() {
    if (!m_running || m_masterEndpoint.sin_family == 0) {
        return;
    }
    const Message message{
        MessageType::SlaveHello,
        encodeSlaveHello(m_peerId.empty() ? "slave" : m_peerId,
                         m_socket.localPort(),
                         SecurityKeyStore::load()),
    };
    const std::vector<uint8_t> bytes = serializeMessage(message);
    if (!m_socket.sendTo(m_masterEndpoint, bytes.data(), bytes.size())) {
        Logger::write("slave: failed to send hello to " + m_masterIp + ":" +
                      std::to_string(m_masterPort));
    }
}

}  // namespace synccontrol
