#pragma once

#include <Windows.h>

#include <functional>
#include <json.hpp>
#include <thread>

namespace synccontrol {

class IpcServer {
public:
    using RequestHandler = std::function<nlohmann::json(const nlohmann::json&)>;

    explicit IpcServer(RequestHandler handler);
    ~IpcServer();

    bool start();
    void stop();

private:
    void run();
    void closePipe();

    RequestHandler m_handler;
    std::thread m_thread;
    HANDLE m_stopEvent = nullptr;
    HANDLE m_pipe = nullptr;
    bool m_running = false;
};

}  // namespace synccontrol
