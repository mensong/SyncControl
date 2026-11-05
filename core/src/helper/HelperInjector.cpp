#include "helper/HelperInjector.h"

#include <sddl.h>

#include "core/Logger.h"

namespace synccontrol {
namespace {

// Local-only pipe. DACL: SYSTEM and Administrators fully; the querying core
// process is elevated, so it qualifies. No world access.
constexpr wchar_t kHelperPipeName[] = L"\\\\.\\pipe\\SyncControlHelper";
constexpr DWORD kBufferSize = 16 * 1024;
// Frames arrive one per WriteFile from the client; the pipe is byte mode so a
// message can be reassembled with the 2-byte length prefix below.
constexpr uint16_t kFrameMaxBytes = 0xFFFF;
constexpr uint32_t kDesktopPollMs = 150;

void putU16(std::vector<uint8_t>& output, uint16_t value) {
    output.push_back(static_cast<uint8_t>(value & 0xFF));
    output.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

uint16_t getU16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8);
}

PSECURITY_DESCRIPTOR helperPipeSecurity() {
    // SDDL: Owner/Creator + SYSTEM + Administrators, Generic All; no inheritance.
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"O:SYG:SYD:PAI(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &descriptor,
        nullptr);
    return descriptor;
}

}  // namespace

bool HelperInjector::start() {
    if (m_running.load()) {
        return true;
    }
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (m_stopEvent == nullptr) {
        return false;
    }
    m_running = true;
    m_thread = std::thread(&HelperInjector::run, this);
    return true;
}

void HelperInjector::stop() {
    if (!m_running.exchange(false)) {
        return;
    }
    if (m_stopEvent != nullptr) {
        SetEvent(m_stopEvent);
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    CloseHandle(m_stopEvent);
    m_stopEvent = nullptr;
}

void HelperInjector::waitForStop() const {
    if (m_stopEvent != nullptr) {
        WaitForSingleObject(m_stopEvent, INFINITE);
    }
}

bool HelperInjector::ensureDesktop() {
    const HDESK inputDesktop = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (inputDesktop == nullptr) {
        return false;
    }

    // Compare by name; CompareObjectHandles is not importable from all
    // toolchain configurations we build with.
    const auto desktopName = [](HANDLE handle) -> std::wstring {
        wchar_t name[64]{};
        DWORD needed = 0;
        if (GetUserObjectInformationW(handle, 2 /*UO_NAME*/, name,
                                      static_cast<DWORD>(sizeof(name)), &needed)) {
            return name;
        }
        return L"?";
    };

    if (m_currentDesktop != nullptr &&
        desktopName(m_currentDesktop) == desktopName(inputDesktop)) {
        CloseDesktop(inputDesktop);
        return true;
    }
    if (m_currentDesktop != nullptr) {
        CloseDesktop(m_currentDesktop);
        m_currentDesktop = nullptr;
    }

    // Attach to the input desktop. CreateWindow/SetSystemCursor inside the
    // injection path require the thread's desktop to match the input one.
    if (!SetThreadDesktop(inputDesktop)) {
        Logger::write("helper: SetThreadDesktop failed " + std::to_string(GetLastError()));
        CloseDesktop(inputDesktop);
        return false;
    }
    m_currentDesktop = inputDesktop;
    Logger::write("helper: attached to input desktop");
    return true;
}

void HelperInjector::run() {
    // Keep the process's default desktop referenced so the initial thread can
    // switch away and back without the desktop closing behind it.
    m_currentProcessDesktop = GetProcessWindowStation() != nullptr
                                  ? GetThreadDesktop(GetCurrentThreadId())
                                  : nullptr;

    const PSECURITY_DESCRIPTOR security = helperPipeSecurity();
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), security, FALSE};

    while (m_running.load()) {
        // FILE_FLAG_OVERLAPPED is required: ConnectNamedPipe below uses an
        // OVERLAPPED so shutdown can interrupt the wait. Without the flag the
        // instance never listens and clients always see ERROR_PIPE_BUSY.
        const HANDLE pipe = CreateNamedPipeW(
            kHelperPipeName,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,
            kBufferSize,
            kBufferSize,
            0,
            &attributes);
        if (pipe == INVALID_HANDLE_VALUE) {
            Logger::write("helper: CreateNamedPipe failed " +
                          std::to_string(GetLastError()));
            const DWORD waitResult =
                WaitForSingleObject(m_stopEvent, 500);
            if (waitResult == WAIT_OBJECT_0) {
                break;
            }
            continue;
        }

        // Connect must not block shutdown: poll the client connection while
        // watching the stop event via overlapped connect + event.
        OVERLAPPED connectOverlapped{};
        connectOverlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        const BOOL connectStarted = ConnectNamedPipe(pipe, &connectOverlapped);
        const DWORD connectError = GetLastError();
        bool connected = false;
        if (connectStarted || connectError == ERROR_PIPE_CONNECTED) {
            connected = true;
        } else if (connectError == ERROR_IO_PENDING) {
            const HANDLE waitHandles[2] = {m_stopEvent, connectOverlapped.hEvent};
            connected = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE) ==
                        WAIT_OBJECT_0 + 1;
        }
        CloseHandle(connectOverlapped.hEvent);
        if (!connected) {
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            break;
        }

        serveClient(pipe);
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }

    if (security != nullptr) {
        LocalFree(security);
    }
    if (m_currentDesktop != nullptr) {
        CloseDesktop(m_currentDesktop);
        m_currentDesktop = nullptr;
    }
}

void HelperInjector::serveClient(HANDLE pipe) {
    Logger::write("helper: client connected");
    std::vector<uint8_t> pending;
    pending.reserve(kBufferSize);
    std::vector<uint8_t> chunk(4096);
    bool desktopOk = ensureDesktop();

    // The handle is overlapped (required for ConnectNamedPipe cancellation),
    // so reads go through an event-backed OVERLAPPED as well.
    OVERLAPPED readOverlapped{};
    readOverlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (readOverlapped.hEvent == nullptr) {
        Logger::write("helper: CreateEvent for read failed");
        return;
    }

    while (m_running.load()) {
        // Desktop watch. OpenInputDesktop succeeds for SYSTEM on both Default
        // and Winlogon; a fail here is transient (desktop switching).
        const bool desktopNow = ensureDesktop();
        if (desktopNow && !desktopOk) {
            Logger::write("helper: desktop reattached");
        }
        desktopOk = desktopNow;

        // Byte-mode pipe: drain whatever arrived, then parse whole frames.
        DWORD read = 0;
        const BOOL readStarted = ReadFile(pipe, chunk.data(),
                                          static_cast<DWORD>(chunk.size()),
                                          &read, &readOverlapped);
        if (!readStarted && GetLastError() != ERROR_IO_PENDING) {
            break;  // client disconnected or pipe broken
        }
        HANDLE waitHandles[2] = {m_stopEvent, readOverlapped.hEvent};
        if (WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE) !=
                WAIT_OBJECT_0 + 1) {
            CancelIoEx(pipe, &readOverlapped);
            break;  // shutting down
        }
        if (!GetOverlappedResult(pipe, &readOverlapped, &read, FALSE)) {
            break;  // client disconnected or pipe broken
        }
        if (read > 0) {
            pending.insert(pending.end(), chunk.begin(), chunk.begin() + read);
        }

        size_t offset = 0;
        bool parseFailed = false;
        while (pending.size() - offset >= 4) {
            const uint16_t length = getU16(pending.data() + offset + 2);
            const size_t frameSize = 4u + length;
            if (pending.size() - offset < frameSize) {
                break;
            }
            const uint8_t* frame = pending.data() + offset;
            Message message{MessageType::MouseMove, {}};
            if (decodeMessage(frame, frameSize, offset, message)) {
                if (desktopOk) {
                    handleFrame(message);
                }
            } else {
                parseFailed = true;
                break;
            }
        }
        if (parseFailed) {
            // Corrupt stream: resync by dropping the buffer.
            pending.clear();
        } else if (offset > 0) {
            pending.erase(pending.begin(), pending.begin() + static_cast<long>(offset));
        }
    }
    Logger::write("helper: client disconnected");
    CloseHandle(readOverlapped.hEvent);
}

bool HelperInjector::handleFrame(const Message& message) {
    switch (message.type) {
        case MessageType::MouseMove: {
            uint16_t x = 0;
            uint16_t y = 0;
            if (!decodeMouseMove(message.payload, x, y)) {
                return false;
            }
            return m_input.mouseMove(x, y);
        }
        case MessageType::MouseButton: {
            MouseButtonCode button = MouseButtonCode::Left;
            MouseAction action = MouseAction::Down;
            if (!decodeMouseButton(message.payload, button, action)) {
                return false;
            }
            return m_input.mouseButton(button, action);
        }
        case MessageType::MouseWheel: {
            WheelDirection direction = WheelDirection::Vertical;
            int16_t delta = 0;
            if (!decodeMouseWheel(message.payload, direction, delta)) {
                return false;
            }
            return m_input.mouseWheel(direction, delta);
        }
        case MessageType::KeyDown:
        case MessageType::KeyUp: {
            uint16_t vkCode = 0;
            uint16_t scanCode = 0;
            bool extendedKey = false;
            if (!decodeKeyEvent(message.payload, vkCode, scanCode, extendedKey)) {
                return false;
            }
            return m_input.keyEvent(message.type == MessageType::KeyDown, vkCode,
                                    scanCode, extendedKey);
        }
        case MessageType::SwitchIn: {
            uint16_t x = 0;
            uint16_t y = 0;
            if (!decodeSwitchIn(message.payload, x, y)) {
                return false;
            }
            return m_input.switchIn(x, y);
        }
        case MessageType::SwitchOut:
            m_input.switchOut();
            return true;
        default:
            return true;
    }
}

}  // namespace synccontrol
