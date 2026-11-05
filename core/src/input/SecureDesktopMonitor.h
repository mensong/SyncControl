#pragma once

#include <atomic>
#include <cstdint>

namespace synccontrol {

// Detects whether the physical console input is on the secure Winlogon
// desktop (UAC prompt, lock screen). Used by the master to release remote
// focus: the low-level hooks cannot see secure-desktop input, so control
// must return to the local user while a UAC prompt is up.
class SecureDesktopMonitor {
public:
    // Starts a background poll (~150ms). Callback fires on every transition;
    // secure=true means the user cannot be controlling a remote screen.
    bool start(void (*onSecureChanged)(bool secure));
    void stop();
    bool isSecure() const { return m_secure.load(); }

private:
    void run();

    void (*m_callback)(bool) = nullptr;
    unsigned long long m_thread = 0;
    bool m_running = false;
    std::atomic<bool> m_secure{false};
};

}  // namespace synccontrol
