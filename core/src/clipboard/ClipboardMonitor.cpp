#include "clipboard/ClipboardMonitor.h"

#include <shellapi.h>
#include <shlobj_core.h>

#include <cstring>
#include <string>
#include <vector>
#include <wchar.h>

namespace synccontrol {
namespace {

constexpr wchar_t kWindowClassName[] = L"SyncControlClipboardWindow";
constexpr int kOpenRetryCount = 3;
constexpr DWORD kOpenRetryDelayMs = 10;
constexpr size_t kMaxClipboardFiles = 100000;
constexpr int kMaxDirectoryDepth = 32;

std::wstring utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                         static_cast<int>(utf8.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        wide.data(), size);
    return wide;
}

std::string wideToUtf8(const wchar_t* data, int chars) {
    if (data == nullptr || chars <= 0) {
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, data, chars,
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return {};
    }
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, data, chars, utf8.data(), bytes,
                        nullptr, nullptr);
    return utf8;
}

std::string wideToUtf8(const std::wstring& value) {
    return wideToUtf8(value.data(), static_cast<int>(value.size()));
}

bool openClipboardWithRetry(HWND owner) {
    for (int attempt = 0; attempt < kOpenRetryCount; ++attempt) {
        if (OpenClipboard(owner)) {
            return true;
        }
        Sleep(kOpenRetryDelayMs);
    }
    return false;
}

uint64_t fileSizeAtPath(const std::wstring& path) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return 0;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) {
        size.QuadPart = 0;
    }
    CloseHandle(file);
    return static_cast<uint64_t>(size.QuadPart);
}

std::wstring leafName(const std::wstring& path) {
    const size_t split = path.find_last_of(L"\\/");
    return split == std::wstring::npos ? path : path.substr(split + 1);
}

// A selected directory is kept as a directory entry; descendants are emitted
// as forward-slash relative names, which matches the wire FileEntry contract.
void appendFileTree(std::vector<FileEntry>& files, const std::wstring& path,
                    const std::string& name, bool isDir, int depth) {
    if (files.size() >= kMaxClipboardFiles || name.empty() || depth > kMaxDirectoryDepth) {
        return;
    }

    FileEntry entry;
    entry.path = wideToUtf8(path);
    entry.name = name;
    entry.isDir = isDir;
    entry.size = isDir ? 0 : fileSizeAtPath(path);
    files.push_back(std::move(entry));
    if (!isDir) {
        return;
    }

    WIN32_FIND_DATAW find{};
    const HANDLE search = FindFirstFileW((path + L"\\*").c_str(), &find);
    if (search == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if (wcscmp(find.cFileName, L".") == 0 || wcscmp(find.cFileName, L"..") == 0 ||
            (find.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            continue;
        }
        const std::wstring childPath = path + L"\\" + find.cFileName;
        const std::string childName = name + "/" + wideToUtf8(find.cFileName);
        const bool childIsDir = (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        appendFileTree(files, childPath, childName, childIsDir, depth + 1);
    } while (FindNextFileW(search, &find));
    FindClose(search);
}

std::vector<FileEntry> buildFileEntries(HDROP drop) {
    std::vector<FileEntry> files;
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT index = 0; index < count && files.size() < kMaxClipboardFiles; ++index) {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        if (length == 0) {
            continue;
        }
        std::wstring path(static_cast<size_t>(length), L'\0');
        if (DragQueryFileW(drop, index, path.data(), length + 1) != length) {
            continue;
        }
        const DWORD attributes = GetFileAttributesW(path.c_str());
        const bool isDir = attributes != INVALID_FILE_ATTRIBUTES &&
                           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        appendFileTree(files, path, wideToUtf8(leafName(path)), isDir, 0);
    }
    return files;
}

bool filesEqual(const std::vector<FileEntry>& left, const std::vector<FileEntry>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t index = 0; index < left.size(); ++index) {
        const FileEntry& a = left[index];
        const FileEntry& b = right[index];
        if (a.path != b.path || a.name != b.name || a.size != b.size || a.isDir != b.isDir) {
            return false;
        }
    }
    return true;
}

HGLOBAL allocUtf8Blob(const std::string& value) {
    const SIZE_T bytes = value.size() + 1;
    const HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (handle == nullptr) {
        return nullptr;
    }
    void* target = GlobalLock(handle);
    if (target == nullptr) {
        GlobalFree(handle);
        return nullptr;
    }
    memcpy(target, value.c_str(), bytes);
    GlobalUnlock(handle);
    return handle;
}

std::string filesMetaText(const std::vector<FileEntry>& files) {
    std::string text;
    for (const FileEntry& entry : files) {
        text += entry.name;
        text += '|';
        text += std::to_string(entry.size);
        text += entry.isDir ? "|dir\n" : "\n";
    }
    return text;
}

// Explorer pastes top-level entries; the received list also contains directory
// descendants, so only names without "/" belong in the rendered HDROP.
HGLOBAL allocHdrop(const std::vector<FileEntry>& files) {
    std::vector<std::wstring> paths;
    for (const FileEntry& entry : files) {
        if (entry.name.find('/') == std::string::npos) {
            paths.push_back(utf8ToWide(entry.path));
        }
    }
    if (paths.empty()) {
        return nullptr;
    }

    SIZE_T bytes = sizeof(DROPFILES);
    for (const std::wstring& path : paths) {
        bytes += (path.size() + 1) * sizeof(wchar_t);
    }
    bytes += sizeof(wchar_t);
    const HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (handle == nullptr) {
        return nullptr;
    }
    auto* header = static_cast<DROPFILES*>(GlobalLock(handle));
    if (header == nullptr) {
        GlobalFree(handle);
        return nullptr;
    }
    header->pFiles = sizeof(DROPFILES);
    header->pt = {};
    header->fNC = FALSE;
    header->fWide = TRUE;
    auto* target = reinterpret_cast<wchar_t*>(header + 1);
    for (const std::wstring& path : paths) {
        memcpy(target, path.c_str(), (path.size() + 1) * sizeof(wchar_t));
        target += path.size() + 1;
    }
    *target = L'\0';
    GlobalUnlock(handle);
    return handle;
}

}  // namespace

ClipboardMonitor* ClipboardMonitor::s_instance = nullptr;

bool ClipboardMonitor::start() {
    if (m_window != nullptr) {
        return true;
    }

    s_instance = this;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = &ClipboardMonitor::windowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClassName;
    m_classAtom = RegisterClassW(&windowClass);
    if (m_classAtom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        stop();
        return false;
    }

    m_window = CreateWindowExW(0, kWindowClassName, L"SyncControlClipboard", 0,
                               0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    if (m_filesFormat == 0) {
        m_filesFormat = RegisterClipboardFormatW(L"SyncControl.Files");
    }
    if (m_window == nullptr || !AddClipboardFormatListener(m_window)) {
        stop();
        return false;
    }
    refresh();
    return true;
}

void ClipboardMonitor::stop() {
    if (m_window != nullptr) {
        RemoveClipboardFormatListener(m_window);
        DestroyWindow(m_window);
        m_window = nullptr;
    }
    if (m_classAtom != 0) {
        UnregisterClassW(MAKEINTRESOURCEW(m_classAtom), GetModuleHandleW(nullptr));
        m_classAtom = 0;
    }
    if (s_instance == this) {
        s_instance = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cache = {};
    }
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pendingRemoteFiles.clear();
    }
    m_filesFormat = 0;
    m_expectedClipboardSequence = 0;
    m_hasPendingRemoteFiles = false;
    m_renderingFiles = false;
}

LRESULT CALLBACK ClipboardMonitor::windowProc(HWND window, UINT message,
                                              WPARAM wParam, LPARAM lParam) {
    if (s_instance != nullptr) {
        return s_instance->handleMessage(message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT ClipboardMonitor::handleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    if (m_window == nullptr) {
        return DefWindowProcW(nullptr, message, wParam, lParam);
    }
    switch (message) {
        case WM_CLIPBOARDUPDATE:
            if (GetClipboardSequenceNumber() != m_expectedClipboardSequence) {
                refresh();
            }
            return 0;
        case WM_RENDERFORMAT:
            return handleRenderFormat(static_cast<UINT>(wParam));
        case WM_DESTROY:
            return 0;
        default:
            return DefWindowProcW(m_window, message, wParam, lParam);
    }
}

bool ClipboardMonitor::readClipboard(Snapshot& out) const {
    out = {};
    if (!openClipboardWithRetry(m_window)) {
        return false;
    }

    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        const HANDLE handle = GetClipboardData(CF_UNICODETEXT);
        if (handle != nullptr) {
            const SIZE_T bytes = GlobalSize(handle);
            const auto* text = static_cast<const wchar_t*>(
                GlobalLock(static_cast<HGLOBAL>(handle)));
            if (text != nullptr && bytes >= sizeof(wchar_t)) {
                const size_t length = wcsnlen(text, bytes / sizeof(wchar_t));
                if (length > 0) {
                    out.state = State::Text;
                    out.text = wideToUtf8(text, static_cast<int>(length));
                }
            }
            if (text != nullptr) {
                GlobalUnlock(static_cast<HGLOBAL>(handle));
            }
        }
    } else if (IsClipboardFormatAvailable(CF_HDROP)) {
        const HANDLE handle = GetClipboardData(CF_HDROP);
        if (handle != nullptr) {
            out.state = State::Files;
            out.files = buildFileEntries(static_cast<HDROP>(handle));
        }
    }
    CloseClipboard();
    return true;
}

bool ClipboardMonitor::storeIfChanged(Snapshot&& snapshot) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (snapshot.state == m_cache.state && snapshot.text == m_cache.text &&
        filesEqual(snapshot.files, m_cache.files)) {
        return false;  // 某些软件高频重写相同内容，去重避免抖动
    }
    m_cache = std::move(snapshot);
    return true;
}

bool ClipboardMonitor::refresh() {
    Snapshot snapshot;
    if (!readClipboard(snapshot)) {
        return false;
    }
    return storeIfChanged(std::move(snapshot));
}

ClipboardMonitor::Snapshot ClipboardMonitor::snapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cache;
}

bool ClipboardMonitor::setTextToClipboard(const std::string& utf8) {
    const std::wstring wide = utf8ToWide(utf8);
    if (wide.empty() || !openClipboardWithRetry(m_window)) {
        return false;
    }

    bool ok = false;
    if (EmptyClipboard()) {
        const SIZE_T bytes = (wide.size() + 1) * sizeof(wchar_t);
        const HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (handle != nullptr) {
            void* target = GlobalLock(handle);
            if (target != nullptr) {
                memcpy(target, wide.c_str(), bytes);
                GlobalUnlock(handle);
                if (SetClipboardData(CF_UNICODETEXT, handle) != nullptr) {
                    ok = true;
                } else {
                    GlobalFree(handle);
                }
            } else {
                GlobalFree(handle);
            }
        }
    }
    CloseClipboard();
    return ok;
}

bool ClipboardMonitor::setPendingFilesMeta(const std::vector<FileEntry>& files) {
    if (m_window == nullptr || files.empty() || !openClipboardWithRetry(m_window)) {
        return false;
    }

    bool ok = false;
    if (EmptyClipboard()) {
        const HGLOBAL metadata = allocUtf8Blob(filesMetaText(files));
        if (metadata != nullptr && SetClipboardData(m_filesFormat, metadata) == nullptr) {
            GlobalFree(metadata);
        }
        // A null handle defers CF_HDROP rendering until the paste target asks.
        if (SetClipboardData(CF_HDROP, nullptr) != nullptr) {
            ok = true;
        }
    }
    CloseClipboard();
    if (!ok) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pendingRemoteFiles = files;
    }
    m_hasPendingRemoteFiles = true;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Snapshot snapshot;
        snapshot.state = State::Files;
        snapshot.files = files;
        m_cache = std::move(snapshot);
    }
    rememberClipboardUpdate();
    return true;
}

bool ClipboardMonitor::setFilesToClipboard(const std::vector<FileEntry>& files) {
    if (m_window == nullptr || files.empty() || !openClipboardWithRetry(m_window)) {
        return false;
    }

    bool ok = false;
    if (EmptyClipboard()) {
        const HGLOBAL drop = allocHdrop(files);
        if (drop != nullptr) {
            if (SetClipboardData(CF_HDROP, drop) != nullptr) {
                ok = true;
            } else {
                GlobalFree(drop);
            }
        }
    }
    CloseClipboard();
    if (!ok) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Snapshot snapshot;
        snapshot.state = State::Files;
        snapshot.files = files;
        m_cache = std::move(snapshot);
    }
    rememberClipboardUpdate();
    return true;
}

void ClipboardMonitor::rememberClipboardUpdate() {
    m_expectedClipboardSequence = GetClipboardSequenceNumber();
}

LRESULT ClipboardMonitor::handleRenderFormat(UINT format) {
    if (format != CF_HDROP || !m_hasPendingRemoteFiles || m_renderingFiles ||
        !m_fileRequest) {
        return 0;
    }

    std::vector<FileEntry> metadata;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        metadata = m_pendingRemoteFiles;
    }
    if (metadata.empty()) {
        m_hasPendingRemoteFiles = false;
        return 0;
    }

    m_renderingFiles = true;
    std::vector<FileEntry> downloaded = m_fileRequest(metadata);
    m_renderingFiles = false;
    m_hasPendingRemoteFiles = false;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pendingRemoteFiles.clear();
    }
    if (downloaded.empty()) {
        return 0;
    }

    const HGLOBAL drop = allocHdrop(downloaded);
    if (drop == nullptr) {
        return 0;
    }
    if (SetClipboardData(CF_HDROP, drop) == nullptr) {
        GlobalFree(drop);
        return 0;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Snapshot snapshot;
        snapshot.state = State::Files;
        snapshot.files = std::move(downloaded);
        m_cache = std::move(snapshot);
    }
    rememberClipboardUpdate();
    return 0;
}

}  // namespace synccontrol
