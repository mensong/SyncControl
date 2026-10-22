#include "ipc/IpcServer.h"

namespace synccontrol {
namespace {

constexpr const char* kPipeName = "\\\\.\\pipe\\SyncControl";
constexpr DWORD kBufferSize = 64 * 1024;

}  // namespace

IpcServer::IpcServer(RequestHandler handler) : m_handler(std::move(handler)) {}

IpcServer::~IpcServer() {
    stop();
}

bool IpcServer::start() {
    if (m_running) {
        return true;
    }

    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (m_stopEvent == nullptr) {
        return false;
    }

    m_running = true;
    m_thread = std::thread(&IpcServer::run, this);
    return true;
}

void IpcServer::stop() {
    if (!m_running) {
        return;
    }

    m_running = false;
    if (m_stopEvent != nullptr) {
        SetEvent(m_stopEvent);
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_stopEvent != nullptr) {
        CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
    }
}

void IpcServer::closePipe() {
    if (m_pipe != nullptr) {
        CancelIoEx(m_pipe, nullptr);
        DisconnectNamedPipe(m_pipe);
        CloseHandle(m_pipe);
        m_pipe = nullptr;
    }
}

void IpcServer::run() {
    while (m_running) {
        m_pipe = CreateNamedPipeA(
            kPipeName,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            kBufferSize,
            kBufferSize,
            0,
            nullptr);
        if (m_pipe == INVALID_HANDLE_VALUE) {
            m_pipe = nullptr;
            if (WaitForSingleObject(m_stopEvent, 200) == WAIT_OBJECT_0) {
                break;
            }
            continue;
        }

        OVERLAPPED connectOverlapped{};
        connectOverlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        const bool connectResult = ConnectNamedPipe(m_pipe, &connectOverlapped) != FALSE ||
                                   GetLastError() == ERROR_PIPE_CONNECTED;
        if (connectResult) {
            SetEvent(connectOverlapped.hEvent);
        } else if (GetLastError() != ERROR_IO_PENDING) {
            CloseHandle(connectOverlapped.hEvent);
            closePipe();
            continue;
        }

        const HANDLE waitHandles[2] = {m_stopEvent, connectOverlapped.hEvent};
        const DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
        if (waitResult != WAIT_OBJECT_0 + 1) {
            CloseHandle(connectOverlapped.hEvent);
            closePipe();
            break;
        }
        CloseHandle(connectOverlapped.hEvent);

        std::vector<char> request(kBufferSize);
        DWORD requestSize = 0;
        bool connected = true;
        while (true) {
            DWORD thisRead = 0;
            const BOOL readResult = ReadFile(m_pipe, request.data() + requestSize,
                                             static_cast<DWORD>(request.size() - requestSize),
                                             &thisRead, nullptr);
            if (!readResult) {
                const DWORD error = GetLastError();
                if (error == ERROR_MORE_DATA && requestSize < request.size()) {
                    requestSize += thisRead;
                    continue;
                }
                connected = false;
                break;
            }
            requestSize += thisRead;
            break;
        }
        if (!connected) {
            closePipe();
            continue;
        }

        // Message-mode pipes return one complete JSON request from ReadFile.
        nlohmann::json response;
        try {
            const nlohmann::json command = nlohmann::json::parse(
                request.begin(), request.begin() + static_cast<std::ptrdiff_t>(requestSize));
            response = m_handler(command);
        } catch (const std::exception& error) {
            response = {{"type", "error"}, {"message", error.what()}};
        }

        const std::string responseText = response.dump();
        WriteFile(m_pipe, responseText.data(), static_cast<DWORD>(responseText.size()),
                  nullptr, nullptr);
        FlushFileBuffers(m_pipe);
        closePipe();
    }
}

}  // namespace synccontrol
