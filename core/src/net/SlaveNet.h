#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "clipboard/ClipboardMonitor.h"
#include "clipboard/FileTransfer.h"
#include "input/InputInject.h"
#include "net/KcpReliable.h"
#include "net/Protocol.h"
#include "net/UdpSocket.h"

namespace synccontrol {

class SlaveNet {
public:
    struct Peer {
        sockaddr_in endpoint{};
        std::string id;
        std::unique_ptr<KcpReliable> kcp;
        uint64_t lastSeenMs = 0;
        uint64_t connectedSinceMs = 0;
        bool authRejected = false;
        bool authFailureLogged = false;
        KcpParameters kcpParameters = kCrossNetworkKcpParameters;
        bool hasMouseSequence = false;
        uint32_t expectedMouseSequence = 0;
        uint64_t mouseReceiveCount = 0;
        uint64_t mouseLossCount = 0;
    };

    struct MouseStats {
        uint64_t received = 0;
        uint64_t lost = 0;
    };

    bool start(uint16_t localPort, const std::string& masterIp, uint16_t masterPort);
    void stop();
    void poll(uint64_t nowMs);
    bool isRunning() const { return m_running; }
    void setKcpOverride(const KcpParameters& parameters);
    void disconnectAll();
    MouseStats mouseStats() const;

    std::vector<Peer>& peers() { return m_peers; }
    const std::vector<Peer>& peers() const { return m_peers; }
    void setClipboardMonitor(ClipboardMonitor* monitor) { m_clipboard = monitor; }
    void setFileTransfer(FileTransferManager* transfer) { m_fileTransfer = transfer; }
    // Target-side paste: asks Master for file data, waits for TCP completion.
    std::vector<FileEntry> requestClipboardFiles();

private:
    Peer* findPeer(const sockaddr_in& endpoint);
    Peer& ensurePeer(const sockaddr_in& endpoint, uint64_t nowMs);
    void removeTimedOutPeers(uint64_t nowMs);
    void receiveUdp(uint64_t nowMs);
    void drainKcp(Peer& peer, uint64_t nowMs);
    void handleFrame(Peer& peer, const Message& message, uint64_t nowMs);
    void handleInputMessage(const Message& message);
    void handleMouseMove(Peer* peer, const Message& message);
    void sendKcpFrame(Peer& peer, const Message& message);
    void sendFilesMeta(Peer& peer, const std::vector<FileEntry>& files);
    void sendHandshakeResponse(Peer& peer);
    void sendHeartbeat(Peer& peer);
    void sendSlaveHello();

    UdpSocket m_socket;
    std::vector<Peer> m_peers;
    InputInject m_input;
    ClipboardMonitor* m_clipboard = nullptr;
    FileTransferManager* m_fileTransfer = nullptr;
    bool m_inputActive = false;
    bool m_running = false;
    std::string m_peerId;
    sockaddr_in m_masterEndpoint{};
    std::string m_masterIp;
    uint16_t m_masterPort = 0;
    uint64_t m_lastHelloMs = 0;
    bool m_kcpOverride = false;
    KcpParameters m_kcpOverrideParameters;
};

}  // namespace synccontrol
