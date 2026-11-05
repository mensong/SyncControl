#include <iostream>

#include <windows.h>

#include <string>
#include <thread>

#include "core/Logger.h"
#include "core/Service.h"
#include "core/TrayIcon.h"
#include "helper/HelperInjector.h"
#include "helper/HelperServiceControl.h"
#include "helper/SessionHelperLauncher.h"

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

// --- --service mode (session 0, LocalSystem) ------------------------------

SERVICE_STATUS_HANDLE g_serviceStatusHandle = nullptr;
SERVICE_STATUS g_serviceStatus{};
HANDLE g_serviceStopEvent = nullptr;
HANDLE g_helperProcess = nullptr;

void reportServiceStatus(DWORD state, DWORD exitCode = 0) {
    if (g_serviceStatusHandle == nullptr) {
        return;
    }
    g_serviceStatus.dwCurrentState = state;
    g_serviceStatus.dwWin32ExitCode = exitCode;
    SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
}

void WINAPI serviceControlHandler(DWORD control) {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        reportServiceStatus(SERVICE_STOP_PENDING);
        SetEvent(g_serviceStopEvent);
    }
}

void WINAPI serviceMain(DWORD, LPWSTR*) {
    g_serviceStatusHandle =
        RegisterServiceCtrlHandlerW(L"SyncControlHelper", serviceControlHandler);
    if (g_serviceStatusHandle == nullptr) {
        return;
    }
    g_serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_serviceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    reportServiceStatus(SERVICE_START_PENDING);

    g_serviceStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_serviceStopEvent == nullptr) {
        reportServiceStatus(SERVICE_STOPPED, 1);
        return;
    }
    reportServiceStatus(SERVICE_RUNNING);

    synccontrol::SessionHelperLauncher::runSupervisorLoop(
        g_serviceStopEvent, &g_helperProcess);

    reportServiceStatus(SERVICE_STOPPED);
}

int runServiceMode() {
    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<LPWSTR>(L"SyncControlHelper"), &serviceMain},
        {nullptr, nullptr},
    };
    if (!StartServiceCtrlDispatcherW(table)) {
        std::cerr << "StartServiceCtrlDispatcher failed " << GetLastError() << "\n";
        return 1;
    }
    return 0;
}

// --- --helper mode (interactive session, SYSTEM token) ---------------------

int runHelperMode() {
    synccontrol::Logger::setHelperMode();
    synccontrol::Logger::write("helper: process starting");
    synccontrol::HelperInjector injector;
    if (!injector.start()) {
        return 1;
    }
    // The supervisor restarts the helper if it exits; a clean wait here keeps
    // the process alive until the supervisor stops it.
    injector.waitForStop();
    injector.stop();
    return 0;
}

// --- --install-service / --uninstall-service (elevated) --------------------

int runInstallUninstall(bool install) {
    std::string error;
    const bool ok = install ? synccontrol::HelperServiceControl::install(error)
                            : synccontrol::HelperServiceControl::uninstall(error);
    if (!ok) {
        synccontrol::Logger::write(std::string("helper: ") +
                                   (install ? "install" : "uninstall") +
                                   " failed: " + error);
        std::cerr << (install ? "install" : "uninstall") << " failed: " << error
                  << "\n";
        return 1;
    }
    std::cout << (install ? "service installed" : "service uninstalled") << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--service") {
            return runServiceMode();
        }
        if (argument == "--helper") {
            // The helper is a child of the service; it must not be blocked by
            // the interactive core's single-instance mutex.
            return runHelperMode();
        }
        if (argument == "--install-service") {
            return runInstallUninstall(true);
        }
        if (argument == "--uninstall-service") {
            return runInstallUninstall(false);
        }
    }

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
