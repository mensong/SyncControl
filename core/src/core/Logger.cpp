#include "core/Logger.h"

#include <Windows.h>

#include <cstdio>
#include <mutex>

namespace synccontrol {
namespace {

constexpr long long kMaxLogBytes = 10LL * 1024 * 1024;

std::wstring logDirectory() {
    char buffer[MAX_PATH]{};
    UINT size = GetEnvironmentVariableA("LOCALAPPDATA", buffer, MAX_PATH);
    if (size == 0 || size > MAX_PATH) {
        size = GetTempPathA(MAX_PATH, buffer);
        if (size == 0 || size > MAX_PATH) {
            return {};
        }
    }
    std::wstring directory(buffer, buffer + size);
    while (!directory.empty() && (directory.back() == L'\\' || directory.back() == L'/')) {
        directory.pop_back();
    }
    directory += L"\\SyncControl";
    CreateDirectoryW(directory.c_str(), nullptr);
    return directory;
}

void writeLocked(const std::wstring& path, const std::string& line) {
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(file);
}

}  // namespace

void Logger::write(const std::string& message) {
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);

    SYSTEMTIME time{};
    GetLocalTime(&time);
    char stamp[40]{};
    std::snprintf(stamp, sizeof(stamp), "[%02u-%02u %02u:%02u:%02u.%03u] ",
                  time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
                  time.wMilliseconds);
    const std::string line = stamp + message + "\n";
    OutputDebugStringA(line.c_str());

    const std::wstring directory = logDirectory();
    if (directory.empty()) {
        return;
    }
    const std::wstring path = directory + L"\\core.log";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        if (GetFileSizeEx(file, &size) && size.QuadPart > kMaxLogBytes) {
            CloseHandle(file);
            const std::wstring old = directory + L"\\core.log.old";
            DeleteFileW(old.c_str());
            if (!MoveFileW(path.c_str(), old.c_str())) {
                DeleteFileW(path.c_str());
            }
            writeLocked(path, line);
            return;
        }
        CloseHandle(file);
    }
    writeLocked(path, line);
}

}  // namespace synccontrol
