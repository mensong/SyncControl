#include "clipboard/ClipboardMonitor.h"

#include <shellapi.h>
#include <shlobj_core.h>

#include <objbase.h>
#include <objidl.h>

#include <cstring>
#include <gdiplus.h>
#include <json.hpp>
#include <string>
#include <vector>
#include <wchar.h>

#include "core/Logger.h"

namespace synccontrol {
namespace {

constexpr wchar_t kWindowClassName[] = L"SyncControlClipboardWindow";
constexpr int kOpenRetryCount = 50;
constexpr int kMetaOpenAttempts = 5;
constexpr DWORD kOpenRetryDelayMs = 20;
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

bool ensureGdiplusStarted(ULONG_PTR& token) {
    if (token != 0) {
        return true;
    }
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) {
        token = 0;
        return false;
    }
    return true;
}

CLSID pngEncoderClsid() {
    CLSID clsid{};
    UINT count = 0;
    UINT savedBytes = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &savedBytes) != Gdiplus::Ok ||
        count == 0 || savedBytes == 0) {
        return clsid;
    }
    std::vector<uint8_t> buffer(static_cast<size_t>(count) *
                                sizeof(Gdiplus::ImageCodecInfo) + savedBytes);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    if (Gdiplus::GetImageEncoders(count, savedBytes, codecs) != Gdiplus::Ok) {
        return clsid;
    }
    for (UINT index = 0; index < count; ++index) {
        if (wcscmp(codecs[index].MimeType, L"image/png") == 0) {
            clsid = codecs[index].Clsid;
            break;
        }
    }
    return clsid;
}

void trimPngToIEnd(std::vector<uint8_t>& png) {
    static const uint8_t kIEnd[8] = {'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82};
    if (png.size() < 8) {
        return;
    }
    for (size_t offset = png.size() - 8;; --offset) {
        if (memcmp(png.data() + offset, kIEnd, 8) == 0) {
            png.resize(offset + 8);
            return;
        }
        if (offset == 0) {
            return;
        }
    }
}

bool parsePngDimensions(const std::vector<uint8_t>& png, uint32_t& width,
                        uint32_t& height) {
    static const uint8_t kPngMagic[8] = {0x89, 'P', 'N', 'G',
                                         0x0D, 0x0A, 0x1A, 0x0A};
    if (png.size() < 24 || memcmp(png.data(), kPngMagic, 8) != 0) {
        return false;
    }
    width = (static_cast<uint32_t>(png[16]) << 24) |
            (static_cast<uint32_t>(png[17]) << 16) |
            (static_cast<uint32_t>(png[18]) << 8) |
            static_cast<uint32_t>(png[19]);
    height = (static_cast<uint32_t>(png[20]) << 24) |
             (static_cast<uint32_t>(png[21]) << 16) |
             (static_cast<uint32_t>(png[22]) << 8) |
             static_cast<uint32_t>(png[23]);
    return width > 0 && height > 0;
}

// Converts a clipboard CF_DIB/CF_DIBV5 blob to lossless PNG via GDI+.
bool dibToPng(const void* dib, size_t dibBytes, std::vector<uint8_t>& png,
              ULONG_PTR gdiplusToken) {
    png.clear();
    if (!ensureGdiplusStarted(gdiplusToken) || dib == nullptr ||
        dibBytes < sizeof(BITMAPINFOHEADER)) {
        return false;
    }
    const auto* info = static_cast<const BITMAPINFO*>(dib);
    const BITMAPINFOHEADER& header = info->bmiHeader;
    if (header.biWidth <= 0 || header.biHeight == 0) {
        return false;
    }
    DWORD paletteEntries = header.biClrUsed;
    if (paletteEntries == 0 && header.biBitCount <= 8) {
        paletteEntries = 1u << header.biBitCount;
    }
    // Legacy 40-byte headers store the bitfield masks after the palette;
    // V5 headers embed them, so only the legacy layout needs the extra skip.
    const size_t maskBytes =
        (header.biCompression == BI_BITFIELDS &&
         header.biSize == sizeof(BITMAPINFOHEADER))
            ? 3 * sizeof(DWORD) : 0;
    const size_t bitsOffset =
        header.biSize + static_cast<size_t>(paletteEntries) * sizeof(RGBQUAD) +
        maskBytes;
    if (dibBytes <= bitsOffset) {
        return false;
    }
    void* bits = const_cast<uint8_t*>(static_cast<const uint8_t*>(dib)) +
                 bitsOffset;
    Gdiplus::Bitmap* bitmap = Gdiplus::Bitmap::FromBITMAPINFO(info, bits);
    if (bitmap == nullptr) {
        return false;
    }
    bool ok = false;
    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(nullptr, TRUE, &stream) == S_OK) {
        const CLSID encoder = pngEncoderClsid();
        if (bitmap->Save(stream, &encoder, nullptr) == Gdiplus::Ok) {
            HGLOBAL global = nullptr;
            if (GetHGlobalFromStream(stream, &global) == S_OK) {
                const SIZE_T bytes = GlobalSize(global);
                const void* data = GlobalLock(global);
                if (data != nullptr && bytes > 0) {
                    png.assign(static_cast<const uint8_t*>(data),
                               static_cast<const uint8_t*>(data) + bytes);
                    GlobalUnlock(global);
                    ok = true;
                }
            }
        }
        stream->Release();
    }
    delete bitmap;
    if (ok) {
        trimPngToIEnd(png);
    } else {
        png.clear();
    }
    return ok && !png.empty();
}

// Decodes PNG into a 32bpp top-down CF_DIB blob (header + pixels).
bool pngToDib32(const std::vector<uint8_t>& png, std::vector<uint8_t>& dib,
                uint32_t& width, uint32_t& height, ULONG_PTR gdiplusToken) {
    dib.clear();
    width = 0;
    height = 0;
    if (!ensureGdiplusStarted(gdiplusToken) || png.empty()) {
        return false;
    }
    HGLOBAL source = GlobalAlloc(GMEM_MOVEABLE, png.size());
    if (source == nullptr) {
        return false;
    }
    void* sourceData = GlobalLock(source);
    if (sourceData == nullptr) {
        GlobalFree(source);
        return false;
    }
    memcpy(sourceData, png.data(), png.size());
    GlobalUnlock(source);
    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(source, TRUE, &stream) != S_OK) {
        GlobalFree(source);
        return false;
    }
    Gdiplus::Bitmap bitmap(stream, FALSE);
    stream->Release();
    if (bitmap.GetLastStatus() != Gdiplus::Ok) {
        return false;
    }
    width = bitmap.GetWidth();
    height = bitmap.GetHeight();
    if (width == 0 || height == 0) {
        return false;
    }
    HBITMAP hbitmap = nullptr;
    if (bitmap.GetHBITMAP(Gdiplus::Color(255, 255, 255, 255), &hbitmap) !=
            Gdiplus::Ok ||
        hbitmap == nullptr) {
        return false;
    }
    const HDC hdc = GetDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    info.bmiHeader.biSizeImage = width * height * 4;
    std::vector<uint8_t> pixels(info.bmiHeader.biSizeImage);
    const int lines = GetDIBits(hdc, hbitmap, 0, static_cast<UINT>(height),
                                pixels.data(), &info, DIB_RGB_COLORS);
    ReleaseDC(nullptr, hdc);
    DeleteObject(hbitmap);
    if (lines != static_cast<int>(height)) {
        return false;
    }
    dib.resize(sizeof(BITMAPINFOHEADER) + pixels.size());
    memcpy(dib.data(), &info.bmiHeader, sizeof(BITMAPINFOHEADER));
    memcpy(dib.data() + sizeof(BITMAPINFOHEADER), pixels.data(), pixels.size());
    return true;
}

HGLOBAL allocRawBlob(const void* data, size_t bytes) {
    if (data == nullptr || bytes == 0) {
        return nullptr;
    }
    const HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (handle == nullptr) {
        return nullptr;
    }
    void* target = GlobalLock(handle);
    if (target == nullptr) {
        GlobalFree(handle);
        return nullptr;
    }
    memcpy(target, data, bytes);
    GlobalUnlock(handle);
    return handle;
}

HGLOBAL allocWideTextBlob(const std::wstring& value) {
    const SIZE_T bytes = (value.size() + 1) * sizeof(wchar_t);
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

// Requires an already-open clipboard.
std::string readUnicodeTextFromClipboard() {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        return {};
    }
    const HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (handle == nullptr) {
        return {};
    }
    const SIZE_T bytes = GlobalSize(handle);
    const auto* text = static_cast<const wchar_t*>(
        GlobalLock(static_cast<HGLOBAL>(handle)));
    std::string result;
    if (text != nullptr && bytes >= sizeof(wchar_t)) {
        const size_t length = wcsnlen(text, bytes / sizeof(wchar_t));
        if (length > 0) {
            result = wideToUtf8(text, static_cast<int>(length));
        }
    }
    if (text != nullptr) {
        GlobalUnlock(static_cast<HGLOBAL>(handle));
    }
    return result;
}

bool openClipboardWithRetry(HWND owner, const char* operation,
                            int attempts = kOpenRetryCount) {
    DWORD lastError = 0;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (OpenClipboard(owner)) {
            return true;
        }
        lastError = GetLastError();
        Sleep(kOpenRetryDelayMs);
    }
    Logger::write(std::string("clipboard: OpenClipboard failed, operation=") +
                  operation + ", attempts=" + std::to_string(attempts) +
                  ", win32Error=" + std::to_string(static_cast<uint32_t>(lastError)));
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

std::string clipboardOriginText(const std::string& originPeerId,
                                uint32_t originClipboardSequence) {
    const nlohmann::json payload = {
        {"origin", originPeerId},
        {"sequence", originClipboardSequence},
    };
    return payload.dump();
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

ClipboardMonitor::~ClipboardMonitor() {
    stop();
}

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
        Logger::write("clipboard: register window class failed, win32Error=" +
                      std::to_string(static_cast<uint32_t>(GetLastError())));
        stop();
        return false;
    }

    const LPCWSTR className = m_classAtom != 0
                                  ? MAKEINTRESOURCEW(m_classAtom)
                                  : kWindowClassName;
    SetLastError(0);
    m_window = CreateWindowExW(0, className, L"SyncControlClipboard", 0,
                               0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    if (m_filesFormat == 0) {
        m_filesFormat = RegisterClipboardFormatW(L"SyncControl.Files");
    }
    if (m_originFormat == 0) {
        m_originFormat = RegisterClipboardFormatW(L"SyncControl.ClipboardOrigin");
    }
    if (m_pngFormat == 0) {
        m_pngFormat = RegisterClipboardFormatW(L"PNG");
    }
    if (m_imagePngFormat == 0) {
        m_imagePngFormat = RegisterClipboardFormatW(L"image/png");
    }
    if (m_htmlFormat == 0) {
        m_htmlFormat = RegisterClipboardFormatW(L"HTML Format");
    }
    if (m_rtfFormat == 0) {
        m_rtfFormat = RegisterClipboardFormatW(L"Rich Text Format");
    }
    if (m_gdiplusToken == 0 && !ensureGdiplusStarted(m_gdiplusToken)) {
        Logger::write("clipboard: gdiplus startup failed, image sync disabled");
    }
    if (m_window == nullptr) {
        const DWORD messageWindowError = GetLastError();
        Logger::write("clipboard: message-only window creation failed, win32Error=" +
                      std::to_string(static_cast<uint32_t>(messageWindowError)));

        // Some restricted or third-party-hooked desktops reject message-only
        // windows. A hidden top-level owner window is equivalent for clipboard
        // notifications and delayed rendering.
        SetLastError(0);
        m_window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, className,
                                   L"", WS_POPUP, 0, 0, 0, 0, nullptr,
                                   nullptr, instance, nullptr);
        if (m_window == nullptr) {
            Logger::write("clipboard: fallback window creation failed, win32Error=" +
                          std::to_string(static_cast<uint32_t>(GetLastError())));
            stop();
            return false;
        }
        Logger::write("clipboard: using hidden top-level fallback window");
    }
    if (!AddClipboardFormatListener(m_window)) {
        Logger::write("clipboard: add clipboard listener failed, win32Error=" +
                      std::to_string(static_cast<uint32_t>(GetLastError())));
        stop();
        return false;
    }
    refresh();
    return true;
}

void ClipboardMonitor::clearRemoteClipboardState() {
    m_hasPendingRemoteFiles = false;
    m_renderingFiles = false;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pendingRemoteFiles.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_actionMutex);
        m_actions.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cache = {};
    }
}

void ClipboardMonitor::clearPendingRemoteFiles() {
    m_hasPendingRemoteFiles = false;
    m_renderingFiles = false;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pendingRemoteFiles.clear();
    }
    // Only wipe the clipboard when it still holds exactly what this window
    // installed; a changed sequence means newer local content must be kept.
    if (m_window == nullptr || GetClipboardOwner() != m_window ||
        GetClipboardSequenceNumber() != m_expectedClipboardSequence) {
        return;
    }
    if (!openClipboardWithRetry(m_window, "clear remote file metadata",
                                kMetaOpenAttempts)) {
        return;
    }
    EmptyClipboard();
    CloseClipboard();
    Logger::write("clipboard: cleared remote file metadata after dismiss");
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
    if (m_gdiplusToken != 0) {
        Gdiplus::GdiplusShutdown(m_gdiplusToken);
        m_gdiplusToken = 0;
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
    {
        std::lock_guard<std::mutex> lock(m_actionMutex);
        m_actions.clear();
    }
    m_filesFormat = 0;
    m_originFormat = 0;
    m_pngFormat = 0;
    m_imagePngFormat = 0;
    m_htmlFormat = 0;
    m_rtfFormat = 0;
    m_expectedClipboardSequence = 0;
    m_hasPendingRemoteFiles = false;
    m_renderingFiles = false;
}

LRESULT CALLBACK ClipboardMonitor::windowProc(HWND window, UINT message,
                                              WPARAM wParam, LPARAM lParam) {
    if (s_instance != nullptr) {
        return s_instance->handleMessage(window, message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT ClipboardMonitor::handleMessage(HWND window, UINT message,
                                        WPARAM wParam, LPARAM lParam) {
    if (m_window == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }
    switch (message) {
        case WM_CLIPBOARDUPDATE:
            if (GetClipboardSequenceNumber() != m_expectedClipboardSequence) {
                refresh();
            }
            return 0;
        case WM_RENDERFORMAT:
            return handleRenderFormat(static_cast<UINT>(wParam));
        case kReinstallRemoteMessage:
            reinstallPendingRemoteFiles();
            return 0;
        case kClipboardActionMessage: {
            std::function<void()> action;
            {
                std::lock_guard<std::mutex> lock(m_actionMutex);
                if (!m_actions.empty()) {
                    action = std::move(m_actions.front());
                    m_actions.erase(m_actions.begin());
                }
            }
            if (action) {
                action();
            }
            return 0;
        }
        case WM_DESTROY:
            return 0;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
    }
}

bool ClipboardMonitor::readClipboard(Snapshot& out) const {
    out = {};
    out.clipboardSequence = GetClipboardSequenceNumber();
    if (!openClipboardWithRetry(m_window, "read local clipboard")) {
        return false;
    }

    if (IsClipboardFormatAvailable(CF_HDROP)) {
        const HANDLE handle = GetClipboardData(CF_HDROP);
        if (handle != nullptr) {
            out.state = State::Files;
            out.files = buildFileEntries(static_cast<HDROP>(handle));
            readClipboardOrigin(out);
        }
    } else if (readImageFromClipboard(out)) {
        // readImageFromClipboard fills state, PNG payload, and dimensions.
    } else if (m_htmlFormat != 0 &&
               readRawFormatFromClipboard(m_htmlFormat, out.htmlData)) {
        out.state = State::Html;
        out.text = readUnicodeTextFromClipboard();
    } else if (m_rtfFormat != 0 &&
               readRawFormatFromClipboard(m_rtfFormat, out.rtfData)) {
        out.state = State::Rtf;
        out.text = readUnicodeTextFromClipboard();
    } else if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        const std::string text = readUnicodeTextFromClipboard();
        if (!text.empty()) {
            out.state = State::Text;
            out.text = text;
        }
    }
    CloseClipboard();
    if (out.state == State::Image) {
        Logger::write("clipboard: detected local image, bytes=" +
                      std::to_string(out.pngData.size()) + ", size=" +
                      std::to_string(out.imageWidth) + "x" +
                      std::to_string(out.imageHeight));
    } else if (out.state == State::Html) {
        Logger::write("clipboard: detected local html, bytes=" +
                      std::to_string(out.htmlData.size()) +
                      ", companionText=" +
                      std::to_string(out.text.size()));
    } else if (out.state == State::Rtf) {
        Logger::write("clipboard: detected local rtf, bytes=" +
                      std::to_string(out.rtfData.size()) +
                      ", companionText=" +
                      std::to_string(out.text.size()));
    }
    if (out.state == State::Files && !out.files.empty()) {
        Logger::write("clipboard: detected local files, count=" +
                      std::to_string(out.files.size()));
    }
    return true;
}

void ClipboardMonitor::readClipboardOrigin(Snapshot& out) const {
    if (out.state != State::Files || m_originFormat == 0 ||
        !IsClipboardFormatAvailable(m_originFormat)) {
        return;
    }

    const HANDLE handle = GetClipboardData(m_originFormat);
    if (handle == nullptr) {
        return;
    }
    const SIZE_T bytes = GlobalSize(handle);
    const auto* text = static_cast<const char*>(
        GlobalLock(static_cast<HGLOBAL>(handle)));
    if (text == nullptr || bytes == 0) {
        if (text != nullptr) {
            GlobalUnlock(static_cast<HGLOBAL>(handle));
        }
        return;
    }

    const size_t length = strnlen(text, bytes);
    if (length != 0) {
        try {
            const nlohmann::json value = nlohmann::json::parse(text, text + length);
            out.originPeerId = value.value("origin", std::string());
            const uint64_t sequence = value.value("sequence", 0ULL);
            out.originClipboardSequence = static_cast<uint32_t>(sequence);
        } catch (const std::exception&) {
            out.originPeerId.clear();
            out.originClipboardSequence = 0;
        }
    }
    GlobalUnlock(static_cast<HGLOBAL>(handle));
}

bool ClipboardMonitor::readImageFromClipboard(Snapshot& out) const {
    for (const UINT format : {m_pngFormat, m_imagePngFormat}) {
        std::vector<uint8_t> png;
        if (format != 0 && readRawFormatFromClipboard(format, png) &&
            parsePngDimensions(png, out.imageWidth, out.imageHeight)) {
            trimPngToIEnd(png);
            out.pngData = std::move(png);
            out.state = State::Image;
            return true;
        }
    }
    for (const UINT format : {static_cast<UINT>(CF_DIBV5), static_cast<UINT>(CF_DIB)}) {
        if (!IsClipboardFormatAvailable(format)) {
            continue;
        }
        const HANDLE handle = GetClipboardData(format);
        if (handle == nullptr) {
            continue;
        }
        const SIZE_T bytes = GlobalSize(handle);
        const void* data = GlobalLock(static_cast<HGLOBAL>(handle));
        std::vector<uint8_t> png;
        const bool converted =
            data != nullptr && dibToPng(data, bytes, png, m_gdiplusToken);
        if (data != nullptr) {
            GlobalUnlock(static_cast<HGLOBAL>(handle));
        }
        if (converted &&
            parsePngDimensions(png, out.imageWidth, out.imageHeight)) {
            out.pngData = std::move(png);
            out.state = State::Image;
            return true;
        }
    }
    return false;
}

bool ClipboardMonitor::readRawFormatFromClipboard(UINT format,
                                                  std::vector<uint8_t>& bytes) const {
    bytes.clear();
    if (format == 0 || !IsClipboardFormatAvailable(format)) {
        return false;
    }
    const HANDLE handle = GetClipboardData(format);
    if (handle == nullptr) {
        return false;
    }
    const SIZE_T size = GlobalSize(handle);
    const void* data = GlobalLock(static_cast<HGLOBAL>(handle));
    if (data == nullptr || size == 0) {
        if (data != nullptr) {
            GlobalUnlock(static_cast<HGLOBAL>(handle));
        }
        return false;
    }
    bytes.assign(static_cast<const uint8_t*>(data),
                 static_cast<const uint8_t*>(data) + size);
    GlobalUnlock(static_cast<HGLOBAL>(handle));
    return !bytes.empty();
}

void ClipboardMonitor::installRemoteSnapshot(
    State state, const std::vector<uint8_t>& richBytes,
    const std::string& companionTextUtf8) {
    std::lock_guard<std::mutex> lock(m_mutex);
    Snapshot snapshot;
    snapshot.state = state;
    snapshot.text = companionTextUtf8;
    switch (state) {
        case State::Html:
            snapshot.htmlData = richBytes;
            break;
        case State::Rtf:
            snapshot.rtfData = richBytes;
            break;
        default:
            break;
    }
    snapshot.clipboardSequence = GetClipboardSequenceNumber();
    m_cache = std::move(snapshot);
}

bool ClipboardMonitor::storeIfChanged(Snapshot&& snapshot) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (snapshot.state == m_cache.state && snapshot.text == m_cache.text &&
        filesEqual(snapshot.files, m_cache.files) &&
        snapshot.pngData == m_cache.pngData &&
        snapshot.imageWidth == m_cache.imageWidth &&
        snapshot.imageHeight == m_cache.imageHeight &&
        snapshot.htmlData == m_cache.htmlData &&
        snapshot.rtfData == m_cache.rtfData &&
        snapshot.originPeerId == m_cache.originPeerId &&
        snapshot.originClipboardSequence == m_cache.originClipboardSequence) {
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

void ClipboardMonitor::postClipboardAction(std::function<void()> action) {
    if (!action || m_window == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_actionMutex);
        m_actions.push_back(std::move(action));
    }
    if (!PostMessageW(m_window, kClipboardActionMessage, 0, 0)) {
        std::lock_guard<std::mutex> lock(m_actionMutex);
        m_actions.clear();
    }
}

bool ClipboardMonitor::setTextToClipboard(const std::string& utf8) {
    const std::wstring wide = utf8ToWide(utf8);
    if (wide.empty() || !openClipboardWithRetry(m_window, "write clipboard text")) {
        return false;
    }

    bool ok = false;
    if (EmptyClipboard()) {
        const HGLOBAL handle = allocWideTextBlob(wide);
        if (handle != nullptr) {
            void* target = GlobalLock(handle);
            if (target != nullptr) {
                memcpy(target, wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
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

bool ClipboardMonitor::setImageToClipboard(const std::vector<uint8_t>& png) {
    std::vector<uint8_t> dib;
    uint32_t width = 0;
    uint32_t height = 0;
    if (png.empty() ||
        !pngToDib32(png, dib, width, height, m_gdiplusToken)) {
        Logger::write("clipboard: failed to decode remote png for clipboard, bytes=" +
                      std::to_string(png.size()));
        return false;
    }
    if (!openClipboardWithRetry(m_window, "write clipboard image")) {
        return false;
    }

    bool ok = false;
    if (EmptyClipboard()) {
        const HGLOBAL dibHandle = allocRawBlob(dib.data(), dib.size());
        if (dibHandle != nullptr &&
            SetClipboardData(CF_DIB, dibHandle) != nullptr) {
            ok = true;
            for (const UINT format : {m_pngFormat, m_imagePngFormat}) {
                if (format == 0) {
                    continue;
                }
                const HGLOBAL pngHandle = allocRawBlob(png.data(), png.size());
                if (pngHandle != nullptr &&
                    SetClipboardData(format, pngHandle) == nullptr) {
                    GlobalFree(pngHandle);
                }
            }
        } else if (dibHandle != nullptr) {
            GlobalFree(dibHandle);
        }
    }
    CloseClipboard();
    if (!ok) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Snapshot snapshot;
        snapshot.state = State::Image;
        snapshot.pngData = png;
        snapshot.imageWidth = width;
        snapshot.imageHeight = height;
        snapshot.clipboardSequence = GetClipboardSequenceNumber();
        m_cache = std::move(snapshot);
    }
    rememberClipboardUpdate();
    Logger::write("clipboard: installed remote image, bytes=" +
                  std::to_string(png.size()) + ", size=" +
                  std::to_string(width) + "x" + std::to_string(height));
    return true;
}

bool ClipboardMonitor::setHtmlToClipboard(const std::vector<uint8_t>& html,
                                          const std::string& companionTextUtf8) {
    if (html.empty() || m_htmlFormat == 0 ||
        !openClipboardWithRetry(m_window, "write clipboard html")) {
        return false;
    }

    bool ok = false;
    if (EmptyClipboard()) {
        const HGLOBAL handle = allocRawBlob(html.data(), html.size());
        if (handle != nullptr) {
            if (SetClipboardData(m_htmlFormat, handle) != nullptr) {
                ok = true;
                if (!companionTextUtf8.empty()) {
                    const HGLOBAL textHandle =
                        allocWideTextBlob(utf8ToWide(companionTextUtf8));
                    if (textHandle != nullptr &&
                        SetClipboardData(CF_UNICODETEXT, textHandle) == nullptr) {
                        GlobalFree(textHandle);
                    }
                }
            } else {
                GlobalFree(handle);
            }
        }
    }
    CloseClipboard();
    if (!ok) {
        return false;
    }
    installRemoteSnapshot(State::Html, html, companionTextUtf8);
    rememberClipboardUpdate();
    Logger::write("clipboard: installed remote html, bytes=" +
                  std::to_string(html.size()) + ", companionText=" +
                  std::to_string(companionTextUtf8.size()));
    return true;
}

bool ClipboardMonitor::setRtfToClipboard(const std::vector<uint8_t>& rtf,
                                         const std::string& companionTextUtf8) {
    if (rtf.empty() || m_rtfFormat == 0 ||
        !openClipboardWithRetry(m_window, "write clipboard rtf")) {
        return false;
    }

    bool ok = false;
    if (EmptyClipboard()) {
        const HGLOBAL handle = allocRawBlob(rtf.data(), rtf.size());
        if (handle != nullptr) {
            if (SetClipboardData(m_rtfFormat, handle) != nullptr) {
                ok = true;
                if (!companionTextUtf8.empty()) {
                    const HGLOBAL textHandle =
                        allocWideTextBlob(utf8ToWide(companionTextUtf8));
                    if (textHandle != nullptr &&
                        SetClipboardData(CF_UNICODETEXT, textHandle) == nullptr) {
                        GlobalFree(textHandle);
                    }
                }
            } else {
                GlobalFree(handle);
            }
        }
    }
    CloseClipboard();
    if (!ok) {
        return false;
    }
    installRemoteSnapshot(State::Rtf, rtf, companionTextUtf8);
    rememberClipboardUpdate();
    Logger::write("clipboard: installed remote rtf, bytes=" +
                  std::to_string(rtf.size()) + ", companionText=" +
                  std::to_string(companionTextUtf8.size()));
    return true;
}

bool ClipboardMonitor::setPendingFilesMeta(
    const std::vector<FileEntry>& files,
    const std::string& originPeerId,
    uint32_t originClipboardSequence) {
    if (m_window == nullptr || files.empty()) {
        Logger::write("clipboard: cannot install remote file metadata, windowValid=" +
                      std::to_string(m_window != nullptr ? 1 : 0) +
                      ", count=" + std::to_string(files.size()));
        return false;
    }
    if (!openClipboardWithRetry(m_window, "install remote file metadata",
                                kMetaOpenAttempts)) {
        Logger::write("clipboard: cannot open clipboard for remote file metadata");
        return false;
    }

    bool ok = false;
    if (EmptyClipboard()) {
        const HGLOBAL metadata = allocUtf8Blob(filesMetaText(files));
        if (metadata != nullptr && SetClipboardData(m_filesFormat, metadata) == nullptr) {
            GlobalFree(metadata);
        }
        if (!originPeerId.empty()) {
            const HGLOBAL origin = allocUtf8Blob(
                clipboardOriginText(originPeerId, originClipboardSequence));
            if (origin != nullptr && SetClipboardData(m_originFormat, origin) == nullptr) {
                GlobalFree(origin);
                Logger::write("clipboard: failed to set clipboard origin format");
            }
        }
        // A null handle defers CF_HDROP rendering until the paste target asks.
        // For delayed rendering this API returns null on success.
        SetLastError(0);
        const HANDLE delayedDrop = SetClipboardData(CF_HDROP, nullptr);
        const DWORD delayedDropError = GetLastError();
        if (delayedDrop != nullptr || delayedDropError == 0) {
            ok = true;
        } else {
            Logger::write("clipboard: SetClipboardData failed for delayed CF_HDROP, win32Error=" +
                          std::to_string(static_cast<uint32_t>(delayedDropError)));
        }
    } else {
        Logger::write("clipboard: EmptyClipboard failed for remote file metadata");
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
        snapshot.clipboardSequence = GetClipboardSequenceNumber();
        snapshot.originPeerId = originPeerId;
        snapshot.originClipboardSequence = originClipboardSequence;
        m_cache = std::move(snapshot);
    }
    rememberClipboardUpdate();
    Logger::write("clipboard: installed remote file metadata, count=" +
                  std::to_string(files.size()));
    return true;
}

bool ClipboardMonitor::setFilesToClipboard(
    const std::vector<FileEntry>& files,
    const std::string& originPeerId,
    uint32_t originClipboardSequence) {
    if (m_window == nullptr || files.empty() ||
        !openClipboardWithRetry(m_window, "write downloaded files")) {
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
        if (ok && !originPeerId.empty()) {
            const HGLOBAL origin = allocUtf8Blob(
                clipboardOriginText(originPeerId, originClipboardSequence));
            if (origin != nullptr && SetClipboardData(m_originFormat, origin) == nullptr) {
                GlobalFree(origin);
                Logger::write("clipboard: failed to set clipboard origin format");
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
        snapshot.clipboardSequence = GetClipboardSequenceNumber();
        snapshot.originPeerId = originPeerId;
        snapshot.originClipboardSequence = originClipboardSequence;
        m_cache = std::move(snapshot);
    }
    m_hasPendingRemoteFiles = false;
    m_renderingFiles = false;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pendingRemoteFiles.clear();
    }
    rememberClipboardUpdate();
    Logger::write("clipboard: installed downloaded files, count=" +
                  std::to_string(files.size()));
    return true;
}

void ClipboardMonitor::rememberClipboardUpdate() {
    m_expectedClipboardSequence = GetClipboardSequenceNumber();
}

LRESULT ClipboardMonitor::handleRenderFormat(UINT format) {
    if (format != CF_HDROP || !m_hasPendingRemoteFiles || m_renderingFiles) {
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
    Logger::write("clipboard: paste requested remote files, count=" +
                  std::to_string(metadata.size()));
    std::vector<FileEntry> downloaded;
    bool ready = false;
    if (m_pasteGate) {
        const RemotePasteResult gate = m_pasteGate(metadata);
        downloaded = gate.files;
        const RemotePasteDecision decision = gate.decision;
        if (decision == RemotePasteDecision::Wait) {
            // The confirm window is still up; keep the delayed item alive.
            PostMessageW(m_window, kReinstallRemoteMessage, 0, 0);
            m_renderingFiles = false;
            return 0;
        }
        ready = decision == RemotePasteDecision::Ready;
        if (decision != RemotePasteDecision::Ready || downloaded.empty()) {
            // The task was cancelled or is gone. Drop the stale clipboard
            // item instead of re-installing it, which would re-trigger the
            // shell's clipboard queries on every right-click.
            clearPendingRemoteFiles();
            m_renderingFiles = false;
            return 0;
        }
    }
    m_renderingFiles = false;

    if (!ready || downloaded.empty()) {
        clearPendingRemoteFiles();
        return 0;
    }

    m_hasPendingRemoteFiles = false;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pendingRemoteFiles.clear();
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

void ClipboardMonitor::reinstallPendingRemoteFiles() {
    if (!m_hasPendingRemoteFiles) {
        return;
    }
    std::vector<FileEntry> metadata;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        metadata = m_pendingRemoteFiles;
    }
    if (!metadata.empty()) {
        setPendingFilesMeta(metadata);
    }
}

}  // namespace synccontrol
