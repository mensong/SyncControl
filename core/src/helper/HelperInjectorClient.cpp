#include "helper/HelperInjectorClient.h"

#include "core/Logger.h"

namespace synccontrol {
namespace {

constexpr wchar_t kHelperPipeName[] = L"\\\\.\\pipe\\SyncControlHelper";

std::vector<uint8_t> frameBytes(const Message& message) {
    std::vector<uint8_t> bytes;
    bytes.reserve(message.payload.size() + 4);
    const uint16_t type = static_cast<uint16_t>(message.type);
    const uint16_t length = static_cast<uint16_t>(message.payload.size());
    bytes.push_back(static_cast<uint8_t>(type & 0xFF));
    bytes.push_back(static_cast<uint8_t>((type >> 8) & 0xFF));
    bytes.push_back(static_cast<uint8_t>(length & 0xFF));
    bytes.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
    bytes.insert(bytes.end(), message.payload.begin(), message.payload.end());
    return bytes;
}

}  // namespace

HelperInjectorClient::~HelperInjectorClient() {
    stopHelperConnection();
}

void HelperInjectorClient::startHelperConnection() {
    if (m_running.exchange(true)) {
        return;
    }
    m_worker = std::thread(&HelperInjectorClient::runConnectionWorker, this);
}

void HelperInjectorClient::stopHelperConnection() {
    if (!m_running.exchange(false)) {
        return;
    }
    if (m_worker.joinable()) {
        m_worker.join();
    }
    std::lock_guard<std::mutex> lock(m_pipeMutex);
    if (m_pipe != nullptr) {
        CloseHandle(m_pipe);
        m_pipe = nullptr;
    }
}

void HelperInjectorClient::runConnectionWorker() {
    while (m_running.load()) {
        bool connected = false;
        {
            std::lock_guard<std::mutex> lock(m_pipeMutex);
            if (m_pipe == nullptr) {
                m_pipe = tryConnect();
                if (m_pipe != nullptr) {
                    m_pipeEverConnected = true;
                    Logger::write("helper-client: connected to session helper");
                }
            }
            connected = m_pipe != nullptr;
        }
        Sleep(connected ? 2000 : 500);
    }
}

HANDLE HelperInjectorClient::tryConnect() const {
    // Do not spam the log: a down helper is the normal state pre-install, so
    // failures are throttled to one line per 10 seconds (not once ever).
    for (int attempt = 0; attempt < 2; ++attempt) {
        const HANDLE pipe = CreateFileW(kHelperPipeName,
                                        GENERIC_READ | GENERIC_WRITE,
                                        0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            // Byte-mode: the helper reads raw frames; no ReadMode change.
            return pipe;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_BUSY && attempt == 0) {
            // The helper serves one client at a time; wait for a free
            // instance instead of giving up for another 500ms.
            WaitNamedPipeW(kHelperPipeName, 200);
            continue;
        }
        const uint64_t nowMs = GetTickCount64();
        if (nowMs - m_lastFailureLogMs >= 10000) {
            Logger::write("helper-client: connect failed error=" +
                          std::to_string(error) +
                          (m_pipeEverConnected ? " (was connected)"
                                               : " (never connected)"));
            m_lastFailureLogMs = nowMs;
        }
        break;
    }
    return nullptr;
}

bool HelperInjectorClient::sendFrame(const Message& message) const {
    // Inject calls can arrive from the network thread and the KCP drain path;
    // serialize pipe access so interleaved writes cannot corrupt frames.
    std::lock_guard<std::mutex> lock(m_pipeMutex);
    if (m_pipe == nullptr) {
        return false;
    }
    HANDLE pipe = m_pipe;

    const std::vector<uint8_t> bytes = frameBytes(message);
    const BOOL sent = WriteFile(pipe, bytes.data(),
                                static_cast<DWORD>(bytes.size()), nullptr, nullptr);
    if (sent) {
        FlushFileBuffers(pipe);
        return true;
    }

    // Broken pipe: drop the handle; the worker reconnects.
    m_pipe = nullptr;
    CloseHandle(pipe);
    Logger::write("helper-client: send failed " + std::to_string(GetLastError()) +
                  ", falling back to local inject");
    return false;
}

bool HelperInjectorClient::mouseMove(uint16_t x, uint16_t y) const {
    if (sendFrame({MessageType::MouseMove, encodeMouseMove(0, x, y)})) {
        return true;
    }
    return m_fallback.mouseMove(x, y);
}

bool HelperInjectorClient::mouseButton(MouseButtonCode button,
                                       MouseAction action) const {
    if (sendFrame({MessageType::MouseButton, encodeMouseButton(button, action)})) {
        return true;
    }
    return m_fallback.mouseButton(button, action);
}

bool HelperInjectorClient::mouseWheel(WheelDirection direction, int16_t delta) const {
    if (sendFrame({MessageType::MouseWheel, encodeMouseWheel(direction, delta)})) {
        return true;
    }
    return m_fallback.mouseWheel(direction, delta);
}

bool HelperInjectorClient::keyEvent(bool down, uint16_t vkCode, uint16_t scanCode,
                                    bool extendedKey) const {
    const std::vector<uint8_t> payload =
        encodeKeyEvent(down ? MessageType::KeyDown : MessageType::KeyUp, vkCode,
                       scanCode, extendedKey);
    if (sendFrame({down ? MessageType::KeyDown : MessageType::KeyUp, payload})) {
        return true;
    }
    return m_fallback.keyEvent(down, vkCode, scanCode, extendedKey);
}

bool HelperInjectorClient::switchIn(uint16_t x, uint16_t y) const {
    if (sendFrame({MessageType::SwitchIn, encodeSwitchIn(x, y)})) {
        return true;
    }
    return m_fallback.switchIn(x, y);
}

void HelperInjectorClient::switchOut() const {
    if (sendFrame({MessageType::SwitchOut, {}})) {
        return;
    }
    m_fallback.switchOut();
}

}  // namespace synccontrol
