#pragma once

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "input/InputInject.h"
#include "net/Protocol.h"

namespace synccontrol {

// Forwards injection frames to the SYSTEM session helper over its local pipe.
// Falls back to direct SendInput when the pipe is unavailable (service not
// installed, UAC-less machines, or helper not yet connected). Thread-safe:
// writes are serialized behind a mutex since the pipe is shared.
class HelperInjectorClient : public InputInject {
public:
    ~HelperInjectorClient() override;

    // Starts the background connection worker. Safe to call once.
    void startHelperConnection();
    void stopHelperConnection();

    bool isHelperConnected() const { return m_pipe != nullptr; }

    bool mouseMove(uint16_t x, uint16_t y) const override;
    bool mouseButton(MouseButtonCode button, MouseAction action) const override;
    bool mouseWheel(WheelDirection direction, int16_t delta) const override;
    bool keyEvent(bool down, uint16_t vkCode, uint16_t scanCode,
                  bool extendedKey) const override;
    bool switchIn(uint16_t x, uint16_t y) const override;
    void switchOut() const override;

private:
    void runConnectionWorker();
    bool sendFrame(const Message& message) const;
    // Attempts a non-blocking connect; returns the live pipe or nullptr.
    HANDLE tryConnect() const;

    // Backing direct-injection fallback (also our base class methods).
    InputInject m_fallback;

    mutable std::mutex m_pipeMutex;
    mutable HANDLE m_pipe = nullptr;
    std::thread m_worker;
    std::atomic<bool> m_running{false};
    // Connect-failure log throttling (one line per 10s, includes the error).
    mutable uint64_t m_lastFailureLogMs = 0;
    mutable bool m_pipeEverConnected = false;
    bool m_fallbackLogged = false;
};

}  // namespace synccontrol
