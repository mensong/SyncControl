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
#include <string>

namespace synccontrol {

class NetworkInitializer;

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    bool open();
    bool bind(uint16_t port);
    bool bind(const std::string& bindIp, uint16_t port);
    void close();

    bool sendTo(const sockaddr_in& destination, const uint8_t* data, size_t size);
    int recvFrom(sockaddr_in& source, uint8_t* data, size_t size);

    uint16_t localPort() const { return m_localPort; }
    bool isOpen() const { return m_socket != INVALID_SOCKET; }

private:
    SOCKET m_socket = INVALID_SOCKET;
    uint16_t m_localPort = 0;
    std::unique_ptr<NetworkInitializer> m_network;
};

// RAII helper shared by UDP and TCP wrappers.
class NetworkInitializer {
public:
    NetworkInitializer();
    ~NetworkInitializer();
};

std::string endpointToString(const sockaddr_in& endpoint);
std::string endpointAddressToString(const sockaddr_in& endpoint);

}  // namespace synccontrol
