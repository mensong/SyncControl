#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>

#include <atomic>
#include <functional>
#include <map>
#include <cstdint>
#include <memory>
#include <mutex>
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
    void setRemoteFilesMetaHandler(
        std::function<void(const std::string& originPeerId,
                           uint32_t originClipboardSequence,
                           const std::vector<FileEntry>& files)> handler) {
        m_remoteFilesMeta = std::move(handler);
    }
    void setRemoteImageMetaHandler(
        std::function<void(const ClipboardImageMeta& meta)> handler) {
        m_remoteImageMeta = std::move(handler);
    }
    void setRemoteRichMetaHandler(
        std::function<void(const ClipboardRichMeta& meta)> handler) {
        m_remoteRichMeta = std::move(handler);
    }

    std::vector<Peer>& peers() { return m_peers; }
    const std::vector<Peer>& peers() const { return m_peers; }
    const std::string& peerId() const { return m_peerId; }
    void setClipboardMonitor(ClipboardMonitor* monitor) { m_clipboard = monitor; }
    void setFileTransfer(FileTransferManager* transfer) { m_fileTransfer = transfer; }
    bool isInputActive() const { return m_inputActive; }
    // Target-side paste: asks Master for file data. The TCP receive session is
    // tracked by FileTransferManager and completed by its callback.
    bool beginClipboardFilesRequest();
    std::vector<FileEntry> requestClipboardImage(const std::string& transferId,
                                                 const std::atomic<bool>& cancel);
    std::vector<FileEntry> requestClipboardRich(const std::string& transferId,
                                                const std::atomic<bool>& cancel);

private:
    Peer* findPeer(const sockaddr_in& endpoint);
    Peer& ensurePeer(const sockaddr_in& endpoint, uint64_t nowMs);
    void removeTimedOutPeers(uint64_t nowMs);
    void receiveUdp(uint64_t nowMs);
    void drainKcp(Peer& peer, uint64_t nowMs);
    void handleFrame(Peer& peer, const Message& message, uint64_t nowMs);
    void handleInputMessage(const Message& message);
    void handleMouseMove(Peer* peer, const Message& message);
    void handleClipboardImageMeta(const ClipboardImageMeta& meta);
    void handleClipboardRichMeta(const ClipboardRichMeta& meta);
    bool sendKcpFrame(Peer& peer, const Message& message);
    void sendFilesMeta(Peer& peer, const std::vector<FileEntry>& files,
                       const std::string& originPeerId,
                       uint32_t originClipboardSequence);
    void sendClipboardImageMeta(Peer& peer, const ClipboardMonitor::Snapshot& snapshot);
    void serveClipboardImageRequest(Peer& peer, const std::string& transferId);
    void sendClipboardRichMeta(Peer& peer, const ClipboardMonitor::Snapshot& snapshot);
    void serveClipboardRichRequest(Peer& peer, const std::string& transferId);
    void sendHandshakeResponse(Peer& peer);
    void sendHeartbeat(Peer& peer);
    void sendSlaveHello();

    UdpSocket m_socket;
    std::vector<Peer> m_peers;
    mutable std::recursive_mutex m_networkMutex;
    InputInject m_input;
    ClipboardMonitor* m_clipboard = nullptr;
    std::function<void(const std::string& originPeerId,
                       uint32_t originClipboardSequence,
                       const std::vector<FileEntry>& files)> m_remoteFilesMeta;
    std::function<void(const ClipboardImageMeta& meta)> m_remoteImageMeta;
    std::function<void(const ClipboardRichMeta& meta)> m_remoteRichMeta;
    FileTransferManager* m_fileTransfer = nullptr;
    std::atomic<bool> m_inputActive = false;
    bool m_running = false;
    std::string m_peerId;
    sockaddr_in m_masterEndpoint{};
    std::string m_masterIp;
    uint16_t m_masterPort = 0;
    uint64_t m_lastHelloMs = 0;
    bool m_kcpOverride = false;
    KcpParameters m_kcpOverrideParameters;

    struct CachedImage {
        std::string path;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t bytes = 0;
    };
    std::map<std::string, CachedImage> m_clipboardImages;
    struct CachedRich {
        ClipboardRichKind kind = ClipboardRichKind::Html;
        std::string richPath;
        std::string textPath;
        uint64_t richBytes = 0;
        uint64_t textBytes = 0;
    };
    std::map<std::string, CachedRich> m_clipboardRich;
};

}  // namespace synccontrol
