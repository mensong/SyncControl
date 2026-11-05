#include "helper/HelperServiceControl.h"

#include <Windows.h>

#include "core/Logger.h"

namespace synccontrol {
namespace {

constexpr wchar_t kServiceName[] = L"SyncControlHelper";
constexpr wchar_t kServiceDisplayName[] = L"SyncControl Session Helper";

std::wstring coreExecutablePath() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD size = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (size == 0 || size >= MAX_PATH) {
        return {};
    }
    return std::wstring(buffer, size);
}

}  // namespace

bool HelperServiceControl::install(std::string& error) {
    const std::wstring path = coreExecutablePath();
    if (path.empty()) {
        error = "cannot resolve core executable path";
        return false;
    }

    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (manager == nullptr) {
        error = "OpenSCManager failed (need administrator): " +
                std::to_string(GetLastError());
        return false;
    }

    // Own service so the SCM lifecycle stays simple. The service process exits
    // on stop; the session helper it spawns is a child and dies with it.
    SC_HANDLE service = CreateServiceW(
        manager, kServiceName, kServiceDisplayName,
        SERVICE_START | SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS,
        SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
        (L"\"" + path + L"\" --service").c_str(), nullptr, nullptr, nullptr,
        nullptr, nullptr);
    if (service == nullptr) {
        const DWORD lastError = GetLastError();
        if (lastError != ERROR_SERVICE_EXISTS) {
            CloseServiceHandle(manager);
            error = "CreateService failed: " + std::to_string(lastError);
            return false;
        }
        // Already installed: reopen with the rights needed to start it.
        service = OpenServiceW(manager, kServiceName,
                               SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS);
        if (service == nullptr) {
            CloseServiceHandle(manager);
            error = "OpenService failed: " + std::to_string(GetLastError());
            return false;
        }
    }
    CloseServiceHandle(manager);

    // Install must leave the helper running; a stopped service means no
    // secure-desktop injection at all.
    SERVICE_STATUS status{};
    if (QueryServiceStatus(service, &status) && status.dwCurrentState != SERVICE_RUNNING) {
        if (!StartServiceW(service, 0, nullptr)) {
            const DWORD lastError = GetLastError();
            if (lastError != ERROR_SERVICE_ALREADY_RUNNING) {
                CloseServiceHandle(service);
                error = "StartService failed: " + std::to_string(lastError);
                return false;
            }
        }
        // Give the SCM time to reach RUNNING so a status query right after
        // install reflects reality.
        for (int waitedMs = 0; waitedMs < 4000; waitedMs += 100) {
            if (!QueryServiceStatus(service, &status)) {
                break;
            }
            if (status.dwCurrentState == SERVICE_RUNNING) {
                break;
            }
            if (status.dwCurrentState == SERVICE_STOPPED) {
                CloseServiceHandle(service);
                error = "service stopped during start (check core.log)";
                return false;
            }
            Sleep(100);
        }
        if (status.dwCurrentState != SERVICE_RUNNING) {
            CloseServiceHandle(service);
            error = "service did not reach RUNNING state";
            return false;
        }
    }
    CloseServiceHandle(service);
    Logger::write("helper: service installed and running");
    return true;
}

bool HelperServiceControl::uninstall(std::string& error) {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        error = "OpenSCManager failed: " + std::to_string(GetLastError());
        return false;
    }
    const SC_HANDLE service = OpenServiceW(
        manager, kServiceName, DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (service == nullptr) {
        const DWORD lastError = GetLastError();
        CloseServiceHandle(manager);
        if (lastError == ERROR_SERVICE_DOES_NOT_EXIST) {
            return true;
        }
        error = "OpenService failed: " + std::to_string(lastError);
        return false;
    }

    SERVICE_STATUS status{};
    ControlService(service, SERVICE_CONTROL_STOP, &status);
    // Wait for the service to actually reach STOPPED before deleting; a
    // delete issued mid-stop leaves the service in MARKED_FOR_DELETE limbo
    // until the process exits.
    for (int waitedMs = 0; waitedMs < 15000; waitedMs += 100) {
        if (!QueryServiceStatus(service, &status) ||
            status.dwCurrentState == SERVICE_STOPPED) {
            break;
        }
        Sleep(100);
    }
    const BOOL deleted = DeleteService(service);
    const DWORD lastError = GetLastError();
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    if (!deleted && lastError != ERROR_SERVICE_MARKED_FOR_DELETE) {
        error = "DeleteService failed: " + std::to_string(lastError);
        return false;
    }
    Logger::write("helper: service uninstalled");
    return true;
}

bool HelperServiceControl::isInstalled() {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        return false;
    }
    const SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_QUERY_STATUS);
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return service != nullptr;
}

bool HelperServiceControl::isRunning() {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        return false;
    }
    const SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_QUERY_STATUS);
    if (service == nullptr) {
        CloseServiceHandle(manager);
        return false;
    }
    SERVICE_STATUS status{};
    const bool running = QueryServiceStatus(service, &status) &&
                         status.dwCurrentState == SERVICE_RUNNING;
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return running;
}

}  // namespace synccontrol
