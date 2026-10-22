#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "net/UdpSocket.h"

namespace synccontrol {

class TcpFileChannel {
public:
    TcpFileChannel() = default;
    explicit TcpFileChannel(SOCKET socket);
    ~TcpFileChannel();

    TcpFileChannel(const TcpFileChannel&) = delete;
    TcpFileChannel& operator=(const TcpFileChannel&) = delete;

    bool connect(const std::string& ip, uint16_t port);
    bool listen(uint16_t port);
    bool listen(const std::string& bindIp, uint16_t port);
    bool accept(TcpFileChannel& client);
    bool sendAll(const uint8_t* data, size_t size);
    // Returns >0 data, 0 orderly close, -1 error, -2 receive timeout.
    int recvSome(uint8_t* data, size_t size);
    void setRecvTimeoutMs(uint32_t milliseconds);
    void setSendTimeoutMs(uint32_t milliseconds);
    void close();
    // Detaches the connected socket without closing it.
    SOCKET detach();
    // Takes ownership of an already-connected socket.
    void adopt(SOCKET socket);

    bool isOpen() const { return m_socket != INVALID_SOCKET; }
    SOCKET handle() const { return m_socket; }

private:
    SOCKET m_socket = INVALID_SOCKET;
    SOCKET m_listener = INVALID_SOCKET;
    std::unique_ptr<NetworkInitializer> m_network;
};

}  // namespace synccontrol
