#pragma once

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <thread>

#include "input/InputInject.h"
#include "net/Protocol.h"

namespace synccontrol {

// Runs inside the interactive session as SYSTEM (spawned by the service).
// Serves a local named pipe carrying SyncControl wire frames and injects them
// with SendInput after attaching to the current input desktop, which follows
// Default <-> Winlogon (UAC, lock screen) transitions.
class HelperInjector {
public:
    bool start();
    void stop();
    // Blocks the calling thread until stop() runs from elsewhere. --helper
    // mode uses this as its main loop.
    void waitForStop() const;

    // Written by the service process so the SCM stop handler can end it.
    void requestStop() { m_running = false; }
    HANDLE stopEvent() const { return m_stopEvent; }

private:
    void run();
    // Attaches this thread to the current input desktop when it changes.
    // Returns false when no input desktop can be opened (never on the
    // interactive session under SYSTEM).
    bool ensureDesktop();
    void serveClient(HANDLE pipe);
    bool handleFrame(const Message& message);

    InputInject m_input;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    HANDLE m_stopEvent = nullptr;
    HDESK m_currentDesktop = nullptr;
    HANDLE m_currentProcessDesktop = nullptr;
};

}  // namespace synccontrol
