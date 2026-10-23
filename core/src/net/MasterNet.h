#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "net/Protocol.h"
#include "net/KcpReliable.h"
#include "net/UdpSocket.h"

namespace synccontrol {

class MasterNet {
public:
    struct PeerInit {
        std::string id;
        std::string ip;
        uint16_t port = 19881;
    };

    struct Peer {
        sockaddr_in endpoint{};
        std::string id;
        std::string ip;
        std::string hostname;
        std::unique_ptr<KcpReliable> kcp;
        bool connected = false;
        uint16_t screenW = 0;
        uint16_t screenH = 0;
        uint16_t filePort = 19881;
        uint64_t lastSeenMs = 0;
        uint64_t lastPacketMs = 0;
        uint64_t lastRawHeartbeatSentMs = 0;
        uint64_t lastKcpSeenMs = 0;
        uint64_t lastHandshakeSentMs = 0;
        uint64_t heartbeatSentMs = 0;
        uint64_t firstAttemptMs = 0;
        uint32_t lastRttMs = 0;
        bool udpWarningSent = false;
        std::string networkType = "cross";
        KcpParameters kcpParameters = kCrossNetworkKcpParameters;
        uint32_t mouseSendSequence = 0;
    };

    bool start(const std::string& bindIp, const std::vector<PeerInit>& peers,
               uint16_t localPort);
    void stop();
    void setPeerId(const std::string& peerId);
    void reauthenticate();
    void poll(uint64_t nowMs);
    bool isRunning() const { return m_running; }
    void setKcpOverride(const KcpParameters& parameters);

    struct ScreenSize {
        bool valid = false;
        uint16_t width = 0;
        uint16_t height = 0;
    };

    ScreenSize firstConnectedScreen() const;
    ScreenSize peerScreenById(const std::string& id) const;
    bool hasPeer(const std::string& id) const;
    void setFocusedPeer(const std::string& id);
    const std::string& focusedPeerId() const { return m_focusPeerId; }
    void queueMouseMove(uint16_t x, uint16_t y);
    bool sendMouseButton(MouseButtonCode button, MouseAction action);
    bool sendMouseWheel(WheelDirection direction, int16_t delta);
    bool sendKeyEvent(bool down, uint16_t vkCode, uint16_t scanCode, bool extendedKey);
    bool sendSwitchIn(uint16_t x, uint16_t y);
    bool sendSwitchInTo(const std::string& peerId, uint16_t x, uint16_t y);
    bool sendSwitchOut();
    bool sendSwitchOutFrom(const std::string& peerId);
    bool sendClipboardTextTo(const std::string& peerId, const std::string& utf8);
    bool sendClipboardRequestTo(const std::string& peerId, bool files = false);
    bool sendFilesMetaTo(const std::string& peerId,
                         const std::vector<FileEntry>& files,
                         const std::string& originPeerId,
                         uint32_t originClipboardSequence);
    void remapPeerIps(const std::vector<PeerInit>& peers);
    uint16_t peerFilePortByIp(const std::string& ip) const;

    // Fires on the Service poll thread when a peer answers CLIPBOARD_REQUEST.
    std::function<void(const std::string& fromPeerId, const std::string& utf8)> onClipboardText;
    std::function<void(const std::string& fromPeerId,
                       const std::string& originPeerId,
                       uint32_t originClipboardSequence,
                       const std::vector<FileEntry>& files)> onClipboardFiles;
    // files=true paste request from a peer. The callback supplies the target IP.
    std::function<void(const std::string& fromPeerId, const std::string& fromPeerIp)>
        onClipboardFilesRequest;

    std::vector<Peer>& peers() { return m_peers; }
    const std::vector<Peer>& peers() const { return m_peers; }

private:
    Peer* findPeer(const sockaddr_in& endpoint);
    Peer* findPeerByIp(const std::string& ip);
    Peer* findPeerById(const std::string& id);
    Peer* firstConnectedPeer();
    Peer* focusedPeer();
    void resetPeer(Peer& peer, uint64_t nowMs);
    void applyNetworkProfile(Peer& peer, bool lan);
    void sendHandshake(Peer& peer);
    void sendRawHeartbeat(Peer& peer, uint64_t nowMs);
    void sendHeartbeat(Peer& peer, uint64_t nowMs);
    void receiveUdp(uint64_t nowMs);
    void drainKcp(Peer& peer, uint64_t nowMs);
    void handleFrame(Peer& peer, const Message& message, uint64_t nowMs);
    bool sendFrame(Peer& peer, const Message& message);
    void flushPendingMove();

    UdpSocket m_socket;
    std::vector<Peer> m_peers;
    mutable std::recursive_mutex m_networkMutex;
    bool m_pendingMouseMove = false;
    uint16_t m_pendingMoveX = 0;
    uint16_t m_pendingMoveY = 0;
    std::string m_focusPeerId;
    bool m_running = false;
    std::string m_peerId = "master";
    bool m_kcpOverride = false;
    KcpParameters m_kcpOverrideParameters;
};

}  // namespace synccontrol
