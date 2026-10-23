#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "ikcp.h"
#include "net/Protocol.h"
#include "net/UdpSocket.h"

namespace synccontrol {

class KcpReliable {
public:
    KcpReliable(UdpSocket& socket, const sockaddr_in& endpoint, IUINT32 conversationId);
    ~KcpReliable();

    KcpReliable(const KcpReliable&) = delete;
    KcpReliable& operator=(const KcpReliable&) = delete;

    bool send(const uint8_t* data, size_t size);
    int recv(uint8_t* data, size_t size);
    int input(const char* data, long size);
    IUINT32 rcvNext() const;
    void update(IUINT32 currentMs);
    void flush();
    void setParameters(const KcpParameters& parameters);
    // Drops all sequence state; used when a restarted peer opens a new stream.
    void resetSession();

private:
    struct OutputContext {
        UdpSocket* socket;
        sockaddr_in endpoint;
        std::vector<uint8_t>* batch;
    };

    static int onKcpOutput(const char* data, int size, ikcpcb* kcp, void* user);
    void flushOutput();

    ikcpcb* m_kcp = nullptr;
    IUINT32 m_conversationId = 0;
    KcpParameters m_parameters;
    UdpSocket* m_socket = nullptr;
    OutputContext m_outputContext{};
    std::vector<uint8_t> m_outputBatch;
};

}  // namespace synccontrol
