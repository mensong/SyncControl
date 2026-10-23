#include "net/KcpReliable.h"

#include "net/Protocol.h"

// Keep only the wire constants; the network profile is supplied by the owner.
constexpr uint16_t kKcpPacketMagic = 0x4B50;

namespace synccontrol {
namespace {

void appendU16(std::vector<uint8_t>& output, uint16_t value) {
    output.push_back(static_cast<uint8_t>(value & 0xFF));
    output.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

}  // namespace

KcpReliable::KcpReliable(UdpSocket& socket, const sockaddr_in& endpoint, IUINT32 conversationId)
    : m_conversationId(conversationId), m_socket(&socket) {
    m_outputContext.socket = &socket;
    m_outputContext.endpoint = endpoint;
    m_outputContext.batch = &m_outputBatch;
    m_kcp = ikcp_create(conversationId, &m_outputContext);
    m_kcp->output = &KcpReliable::onKcpOutput;
    setParameters(kLanKcpParameters);
    ikcp_setmtu(m_kcp, 1200);
}

KcpReliable::~KcpReliable() {
    if (m_kcp != nullptr) {
        ikcp_release(m_kcp);
    }
}

bool KcpReliable::send(const uint8_t* data, size_t size) {
    return m_kcp != nullptr &&
           ikcp_send(m_kcp, reinterpret_cast<const char*>(data), static_cast<int>(size)) >= 0;
}

int KcpReliable::recv(uint8_t* data, size_t size) {
    if (m_kcp == nullptr) {
        return -1;
    }
    return ikcp_recv(m_kcp, reinterpret_cast<char*>(data), static_cast<int>(size));
}

int KcpReliable::input(const char* data, long size) {
    if (m_kcp == nullptr) {
        return -1;
    }
    return ikcp_input(m_kcp, data, size);
}

IUINT32 KcpReliable::rcvNext() const {
    return m_kcp != nullptr ? m_kcp->rcv_nxt : 0;
}

void KcpReliable::update(IUINT32 currentMs) {
    if (m_kcp != nullptr) {
        ikcp_update(m_kcp, currentMs);
        flushOutput();
    }
}

void KcpReliable::flush() {
    update(static_cast<IUINT32>(GetTickCount64() & 0xFFFFFFFFU));
}

void KcpReliable::setParameters(const KcpParameters& parameters) {
    m_parameters = parameters;
    if (m_kcp == nullptr) {
        return;
    }
    ikcp_nodelay(m_kcp, parameters.nodelay, parameters.interval, parameters.resend,
                 parameters.nc);
    ikcp_wndsize(m_kcp, parameters.sendWindow, parameters.receiveWindow);
}

void KcpReliable::resetSession() {
    if (m_kcp != nullptr) {
        ikcp_release(m_kcp);
    }
    m_outputBatch.clear();
    m_kcp = ikcp_create(m_conversationId, &m_outputContext);
    m_kcp->output = &KcpReliable::onKcpOutput;
    setParameters(m_parameters);
    ikcp_setmtu(m_kcp, 1200);
}

int KcpReliable::onKcpOutput(const char* data, int size, ikcpcb* /*kcp*/, void* user) {
    if (user == nullptr || data == nullptr || size <= 0) {
        return -1;
    }

    OutputContext* context = static_cast<OutputContext*>(user);
    std::vector<uint8_t>& batch = *context->batch;
    batch.reserve(batch.size() + static_cast<size_t>(size) + 2);
    appendU16(batch, kKcpPacketMagic);
    batch.insert(batch.end(), data, data + size);
    return 0;
}

void KcpReliable::flushOutput() {
    if (m_outputBatch.empty()) {
        return;
    }
    const bool sent = m_socket->sendTo(m_outputContext.endpoint, m_outputBatch.data(),
                                       m_outputBatch.size());
    m_outputBatch.clear();
    (void)sent;
}

}  // namespace synccontrol
