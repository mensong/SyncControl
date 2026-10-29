#include <iostream>

#include <windows.h>

#include "core/Service.h"
#include "core/TrayIcon.h"

namespace {

class SingleInstanceMutex {
public:
    SingleInstanceMutex()
        : m_handle(CreateMutexW(nullptr, TRUE, L"Global\\SyncControl.Core.Instance")) {
        if (m_handle && GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(m_handle);
            m_handle = nullptr;
        }
    }

    ~SingleInstanceMutex() {
        if (m_handle) {
            ReleaseMutex(m_handle);
            CloseHandle(m_handle);
        }
    }

    bool acquired() const { return m_handle != nullptr; }

private:
    HANDLE m_handle = nullptr;
};

}  // namespace

int main(int argc, char** argv) {
    SingleInstanceMutex instanceMutex;
    if (!instanceMutex.acquired()) {
        std::cerr << "Another SyncControlCore instance is already running\n";
        return 3;
    }

    synccontrol::Service service;
    bool master = false;
    bool slave = false;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--master") {
            master = true;
        } else if (argument == "--slave") {
            slave = true;
        }
    }

    if (master && slave) {
        std::cerr << "Choose either --master or --slave\n";
        return 2;
    }
    const bool started = master ? service.startSavedService("master")
                       : slave ? service.startSavedService("slave")
                               : service.startSavedService();

    if (!started) {
        std::cerr << "Failed to start saved service mode\n";
        return 1;
    }

    synccontrol::TrayIcon trayIcon;
    if (!trayIcon.start(GetModuleHandleW(nullptr), L"SyncControl Core 服务运行中",
                        GetCurrentThreadId())) {
        std::cerr << "Failed to create the Core tray icon\n";
    }

    service.run();
    trayIcon.stop();
    return 0;
}
