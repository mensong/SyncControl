#include "net/UdpSocket.h"

#include <atomic>
#include <limits>

namespace synccontrol {
namespace {

std::atomic<int> g_networkRefCount{0};

void acquireNetwork() {
    if (g_networkRefCount.fetch_add(1) == 0) {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
    }
}

void releaseNetwork() {
    if (g_networkRefCount.fetch_sub(1) == 1) {
        WSACleanup();
    }
}

}  // namespace

NetworkInitializer::NetworkInitializer() {
    acquireNetwork();
}

NetworkInitializer::~NetworkInitializer() {
    releaseNetwork();
}

UdpSocket::~UdpSocket() {
    close();
}

bool UdpSocket::open() {
    if (m_socket != INVALID_SOCKET) {
        return true;
    }

    m_network = std::make_unique<NetworkInitializer>();
    SOCKET socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socketHandle == INVALID_SOCKET) {
        m_network.reset();
        return false;
    }

    BOOL reuseAddress = TRUE;
    setsockopt(socketHandle, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuseAddress), sizeof(reuseAddress));

    u_long nonBlocking = 1;
    if (ioctlsocket(socketHandle, FIONBIO, &nonBlocking) == SOCKET_ERROR) {
        closesocket(socketHandle);
        m_network.reset();
        return false;
    }

    m_socket = socketHandle;
    return true;
}

bool UdpSocket::bind(uint16_t port) {
    return bind("0.0.0.0", port);
}

bool UdpSocket::bind(const std::string& bindIp, uint16_t port) {
    if (!open()) {
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    if (InetPtonA(AF_INET, bindIp.c_str(), &address.sin_addr) != 1) {
        return false;
    }
    address.sin_port = htons(port);
    if (::bind(m_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        return false;
    }

    int addressLength = sizeof(address);
    if (getsockname(m_socket, reinterpret_cast<sockaddr*>(&address), &addressLength) != SOCKET_ERROR) {
        m_localPort = ntohs(address.sin_port);
    }
    return true;
}

void UdpSocket::close() {
    if (m_socket != INVALID_SOCKET) {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
        m_localPort = 0;
        m_network.reset();
    }
}

bool UdpSocket::sendTo(const sockaddr_in& destination, const uint8_t* data, size_t size) {
    if (!isOpen() || size == 0 || size > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return false;
    }

    const int sent = ::sendto(m_socket, reinterpret_cast<const char*>(data),
                              static_cast<int>(size), 0,
                              reinterpret_cast<const sockaddr*>(&destination),
                              sizeof(destination));
    return sent == static_cast<int>(size);
}

int UdpSocket::recvFrom(sockaddr_in& source, uint8_t* data, size_t size) {
    if (!isOpen()) {
        return -1;
    }

    int sourceLength = sizeof(source);
    const int received = ::recvfrom(m_socket, reinterpret_cast<char*>(data),
                                    static_cast<int>(size), 0,
                                    reinterpret_cast<sockaddr*>(&source), &sourceLength);
    if (received == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        return error == WSAEWOULDBLOCK ? 0 : -1;
    }
    return received;
}

std::string endpointToString(const sockaddr_in& endpoint) {
    char address[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &endpoint.sin_addr, address, sizeof(address));
    return std::string(address) + ":" + std::to_string(ntohs(endpoint.sin_port));
}

std::string endpointAddressToString(const sockaddr_in& endpoint) {
    char address[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &endpoint.sin_addr, address, sizeof(address));
    return address;
}

bool resolveHostIpv4(const std::string& host, in_addr& address) {
    if (InetPtonA(AF_INET, host.c_str(), &address) == 1) {
        return true;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* results = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &results) != 0 ||
        results == nullptr) {
        return false;
    }

    bool resolved = false;
    for (const addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
        if (entry->ai_family == AF_INET &&
            entry->ai_addrlen >= sizeof(sockaddr_in)) {
            address = reinterpret_cast<const sockaddr_in*>(entry->ai_addr)->sin_addr;
            resolved = true;
            break;
        }
    }
    freeaddrinfo(results);
    return resolved;
}

}  // namespace synccontrol
