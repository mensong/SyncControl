#include "helper/SessionHelperLauncher.h"

#include <Windows.h>
#include <WtsApi32.h>

#include <string>

#include "core/Logger.h"

namespace synccontrol {
namespace {

constexpr int kRestartGraceMs = 2000;

std::wstring coreExecutablePath() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD size = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (size == 0 || size >= MAX_PATH) {
        return {};
    }
    return std::wstring(buffer, size);
}

// SYSTEM can query the active console session through WTS. The project builds
// without UNICODE, so pin the A variants explicitly.
DWORD activeConsoleSessionId() {
    PWTS_SESSION_INFOA sessions = nullptr;
    DWORD count = 0;
    DWORD sessionId = 0xFFFFFFFF;
    if (WTSEnumerateSessionsA(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) {
        for (DWORD index = 0; index < count; ++index) {
            if (sessions[index].State == WTSActive) {
                sessionId = sessions[index].SessionId;
                break;
            }
        }
        WTSFreeMemory(sessions);
    }
    if (sessionId == 0xFFFFFFFF) {
        sessionId = WTSGetActiveConsoleSessionId();
    }
    return sessionId;
}

bool startHelperInSession(DWORD sessionId, HANDLE& processOut) {
    const std::wstring path = coreExecutablePath();
    if (path.empty()) {
        Logger::write("helper: cannot resolve executable path");
        return false;
    }

    HANDLE currentToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY,
                          &currentToken)) {
        Logger::write("helper: OpenProcessToken failed " +
                      std::to_string(GetLastError()));
        return false;
    }

    HANDLE primaryToken = nullptr;
    const BOOL duplicated = DuplicateTokenEx(
        currentToken, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation,
        TokenPrimary, &primaryToken);
    CloseHandle(currentToken);
    if (!duplicated) {
        Logger::write("helper: DuplicateTokenEx failed " +
                      std::to_string(GetLastError()));
        return false;
    }

    // Re-anchor the SYSTEM token into the interactive session; SendInput and
    // desktop access are granted per session.
    if (!SetTokenInformation(primaryToken, TokenSessionId, &sessionId,
                             sizeof(sessionId))) {
        Logger::write("helper: SetTokenInformation(SessionId) failed " +
                      std::to_string(GetLastError()));
        CloseHandle(primaryToken);
        return false;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
    PROCESS_INFORMATION processInfo{};
    std::wstring commandLine = L"\"" + path + L"\" --helper";
    const BOOL created = CreateProcessAsUserW(
        primaryToken, path.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr,
        &startup, &processInfo);
    CloseHandle(primaryToken);
    if (!created) {
        Logger::write("helper: CreateProcessAsUser failed " +
                      std::to_string(GetLastError()));
        return false;
    }
    CloseHandle(processInfo.hThread);
    processOut = processInfo.hProcess;
    return true;
}

}  // namespace

bool SessionHelperLauncher::runSupervisorLoop(HANDLE stopEvent,
                                              HANDLE* helperProcessOut) {
    *helperProcessOut = nullptr;
    Logger::write("helper: supervisor started");

    while (true) {
        // Re-query on every (re)start: the console session changes across
        // logon/logoff, and at boot time there may be none yet.
        const DWORD sessionId = activeConsoleSessionId();
        if (sessionId == 0xFFFFFFFF) {
            const DWORD wait = WaitForSingleObject(stopEvent, kRestartGraceMs);
            if (wait == WAIT_OBJECT_0) {
                return true;
            }
            continue;
        }

        HANDLE process = nullptr;
        if (!startHelperInSession(sessionId, process)) {
            Logger::write("helper: failed to start session helper, retrying");
            const DWORD wait = WaitForSingleObject(stopEvent, kRestartGraceMs);
            if (wait == WAIT_OBJECT_0) {
                return true;
            }
            continue;
        }
        *helperProcessOut = process;
        Logger::write("helper: session helper started (session " +
                      std::to_string(sessionId) + ")");

        // Wait on the helper AND the stop event: blocking only on the process
        // makes the service un-stoppable while the helper keeps running.
        const HANDLE waitHandles[2] = {stopEvent, process};
        const DWORD wait =
            WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0) {
            TerminateProcess(process, 0);
            WaitForSingleObject(process, 5000);
            CloseHandle(process);
            *helperProcessOut = nullptr;
            Logger::write("helper: stopped service, helper terminated");
            return true;
        }
        DWORD exitCode = 0;
        GetExitCodeProcess(process, &exitCode);
        CloseHandle(process);
        *helperProcessOut = nullptr;
        Logger::write("helper: session helper exited (" +
                      std::to_string(exitCode) + ")");

        if (WaitForSingleObject(stopEvent, kRestartGraceMs) == WAIT_OBJECT_0) {
            return true;
        }
        Logger::write("helper: restarting session helper");
    }
}

}  // namespace synccontrol
