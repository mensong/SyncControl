#include "input/SecureDesktopMonitor.h"

#include <Windows.h>

#include <thread>

#include "core/Logger.h"

namespace synccontrol {
namespace {

// The master process is elevated but runs in the user session; OpenInputDesktop
// succeeds for Default and fails (access denied / does not exist for the
// caller's window station) while input sits on Winlogon.
bool inputDesktopAvailable() {
    HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (desktop == nullptr) {
        return false;
    }
    CloseDesktop(desktop);
    return true;
}

}  // namespace

bool SecureDesktopMonitor::start(void (*onSecureChanged)(bool secure)) {
    if (m_running) {
        return true;
    }
    m_callback = onSecureChanged;
    m_running = true;
    m_thread = reinterpret_cast<unsigned long long>(
        CreateThread(nullptr, 0,
                     [](LPVOID param) -> DWORD {
                         static_cast<SecureDesktopMonitor*>(param)->run();
                         return 0;
                     },
                     this, 0, nullptr));
    return m_thread != 0;
}

void SecureDesktopMonitor::stop() {
    if (!m_running) {
        return;
    }
    m_running = false;
    const HANDLE thread = reinterpret_cast<HANDLE>(m_thread);
    if (thread != nullptr) {
        WaitForSingleObject(thread, 1000);
        CloseHandle(thread);
    }
    m_thread = 0;
    m_secure = false;
}

void SecureDesktopMonitor::run() {
    while (m_running) {
        const bool secure = !inputDesktopAvailable();
        if (secure != m_secure) {
            m_secure = secure;
            Logger::write(secure ? "input: secure desktop active (UAC/lock)"
                                 : "input: default desktop restored");
            if (m_callback != nullptr) {
                m_callback(secure);
            }
        }
        Sleep(150);
    }
}

}  // namespace synccontrol
