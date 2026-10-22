#include "net/TcpFileChannel.h"

namespace synccontrol {

TcpFileChannel::TcpFileChannel(SOCKET socket) : m_socket(socket) {}

TcpFileChannel::~TcpFileChannel() {
    close();
}

bool TcpFileChannel::connect(const std::string& ip, uint16_t port) {
    if (m_socket != INVALID_SOCKET) {
        return true;
    }

    m_network = std::make_unique<NetworkInitializer>();
    m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_socket == INVALID_SOCKET) {
        m_network.reset();
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1) {
        close();
        return false;
    }
    if (::connect(m_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        close();
        return false;
    }
    return true;
}

bool TcpFileChannel::listen(uint16_t port) {
    return listen("0.0.0.0", port);
}

bool TcpFileChannel::listen(const std::string& bindIp, uint16_t port) {
    if (m_listener != INVALID_SOCKET) {
        return true;
    }

    m_network = std::make_unique<NetworkInitializer>();
    m_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listener == INVALID_SOCKET) {
        m_network.reset();
        return false;
    }

    BOOL reuseAddress = TRUE;
    setsockopt(m_listener, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuseAddress), sizeof(reuseAddress));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    if (inet_pton(AF_INET, bindIp.c_str(), &address.sin_addr) != 1) {
        closesocket(m_listener);
        m_listener = INVALID_SOCKET;
        return false;
    }
    address.sin_port = htons(port);
    if (::bind(m_listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        ::listen(m_listener, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(m_listener);
        m_listener = INVALID_SOCKET;
        return false;
    }
    return true;
}

bool TcpFileChannel::accept(TcpFileChannel& client) {
    if (m_listener == INVALID_SOCKET) {
        return false;
    }
    const SOCKET accepted = ::accept(m_listener, nullptr, nullptr);
    if (accepted == INVALID_SOCKET) {
        return false;
    }
    client.close();
    client.m_socket = accepted;
    client.m_network = std::make_unique<NetworkInitializer>();
    return true;
}

SOCKET TcpFileChannel::detach() {
    const SOCKET socket = m_socket;
    m_socket = INVALID_SOCKET;
    return socket;
}

void TcpFileChannel::adopt(SOCKET socket) {
    close();
    m_socket = socket;
    if (socket != INVALID_SOCKET && !m_network) {
        m_network = std::make_unique<NetworkInitializer>();
    }
}

bool TcpFileChannel::sendAll(const uint8_t* data, size_t size) {
    if (!isOpen()) {
        return false;
    }

    size_t sent = 0;
    while (sent < size) {
        const int chunk = ::send(m_socket, reinterpret_cast<const char*>(data + sent),
                                 static_cast<int>(size - sent), 0);
        if (chunk == SOCKET_ERROR) {
            return false;
        }
        sent += static_cast<size_t>(chunk);
    }
    return true;
}

int TcpFileChannel::recvSome(uint8_t* data, size_t size) {
    if (!isOpen()) {
        return -1;
    }
    const int received = ::recv(m_socket, reinterpret_cast<char*>(data),
                                static_cast<int>(size), 0);
    if (received != SOCKET_ERROR) {
        return received;
    }
    return WSAGetLastError() == WSAETIMEDOUT ? -2 : -1;
}

void TcpFileChannel::setRecvTimeoutMs(uint32_t milliseconds) {
    if (isOpen()) {
        const DWORD timeout = milliseconds;
        setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    }
}

void TcpFileChannel::setSendTimeoutMs(uint32_t milliseconds) {
    if (isOpen()) {
        const DWORD timeout = milliseconds;
        setsockopt(m_socket, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    }
}

void TcpFileChannel::close() {
    if (m_socket != INVALID_SOCKET) {
        shutdown(m_socket, SD_BOTH);
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }
    if (m_listener != INVALID_SOCKET) {
        closesocket(m_listener);
        m_listener = INVALID_SOCKET;
    }
    m_network.reset();
}

}  // namespace synccontrol
