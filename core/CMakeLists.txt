cmake_minimum_required(VERSION 3.20)
project(SyncControlCore LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(SyncControlCore
    src/core/app.manifest
    src/main.cpp
    src/input/InputCapture.cpp
    src/input/InputInject.cpp
    src/clipboard/ClipboardMonitor.cpp
    src/clipboard/FileTransfer.cpp
    src/clipboard/TransferQueue.cpp
    src/layout/ScreenLayout.cpp
    src/layout/EdgeRouter.cpp
    src/net/Protocol.cpp
    src/net/UdpSocket.cpp
    src/net/KcpReliable.cpp
    src/net/TcpFileChannel.cpp
    src/net/MasterNet.cpp
    src/net/SlaveNet.cpp
    src/core/Service.cpp
    src/core/ConfirmWindow.cpp
    src/core/TransferProgressWindow.cpp
    src/core/Security.cpp
    src/core/Logger.cpp
    src/core/TrayIcon.cpp
    src/ipc/IpcServer.cpp
    third_party/kcp/ikcp.c
)

target_include_directories(SyncControlCore PRIVATE
    src
    third_party/kcp
    third_party/nlohmann/json
)

target_compile_definitions(SyncControlCore PRIVATE
    _CRT_SECURE_NO_WARNINGS
    _WINSOCK_DEPRECATED_NO_WARNINGS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(MSVC)
    target_compile_options(SyncControlCore PRIVATE /W4 /utf-8 /permissive-)
endif()

if(MSVC)
    target_link_options(SyncControlCore PRIVATE
        "/SUBSYSTEM:WINDOWS" "/ENTRY:mainCRTStartup" "/MANIFESTUAC:NO")
endif()

target_link_libraries(SyncControlCore PRIVATE ws2_32 user32 shell32 bcrypt advapi32 comctl32)
