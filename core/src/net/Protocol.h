#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace synccontrol {

enum class MessageType : uint16_t {
    HandshakeRequest = 0x01,
    HandshakeResponse = 0x02,
    Heartbeat = 0x03,

    MouseMove = 0x10,
    MouseButton = 0x11,
    MouseWheel = 0x12,

    KeyDown = 0x20,
    KeyUp = 0x21,

    SwitchIn = 0x30,
    SwitchOut = 0x31,

    ClipboardText = 0x40,
    ClipboardFilesMeta = 0x41,
    ClipboardFileData = 0x42,
    ClipboardRequest = 0x43,

    // Slave-initiated discovery. A configured peer can still use the original
    // master-initiated KCP handshake; this makes an unconfigured slave show up.
    SlaveHello = 0x50,
};

enum class Channel {
    UdpRaw,
    Kcp,
    Tcp,
};

enum class MouseButtonCode : uint8_t {
    Left = 0,
    Right = 1,
    Middle = 2,
    X1 = 3,
    X2 = 4,
};

enum class MouseAction : uint8_t {
    Down = 0,
    Up = 1,
};

enum class WheelDirection : uint8_t {
    Vertical = 0,
    Horizontal = 1,
};

struct Message {
    MessageType type;
    std::vector<uint8_t> payload;
};

// Tuned profiles use low latency first. Manual settings can override them at
// runtime; changing these values on an existing KCP session does not disconnect.
struct KcpParameters {
    int nodelay = 1;
    int interval = 10;
    int resend = 2;
    int nc = 1;
    int sendWindow = 128;
    int receiveWindow = 128;
};

constexpr KcpParameters kLanKcpParameters{1, 10, 2, 1, 128, 128};
constexpr KcpParameters kCrossNetworkKcpParameters{1, 20, 3, 1, 64, 64};

// One clipboard file or directory entry. path is the local absolute path on
// the machine that owns the clipboard; name is the wire-safe relative name
// using forward slashes. Directory entries carry size 0 and list their
// children as additional entries ("dir/child.txt").
struct FileEntry {
    std::string path;
    std::string name;
    uint64_t size = 0;
    bool isDir = false;
};

// KCP datagrams share the UDP port with raw protocol frames. This prefix makes
// the two packet families unambiguous on receive.
constexpr uint16_t kKcpPacketMagic = 0x4B50;
constexpr uint16_t kProtocolMagic = 0x4B50;

std::vector<uint8_t> serializeMessage(const Message& message);

// Returns false when the buffer contains no complete frame. On success, offset
// points just beyond the consumed frame so multiple frames can be decoded.
bool decodeMessage(const uint8_t* data, size_t size, size_t& offset, Message& message);

Channel routeChannel(MessageType type);

std::vector<uint8_t> encodeHandshakeRequest(const std::string& peerId,
                                            const std::string& presharedKey);
// Returns the SHA-256 hex digest sent on the wire, not the key itself.
bool decodeHandshakeRequest(const std::vector<uint8_t>& payload,
                            std::string& peerId,
                            std::string& keyHash);

std::vector<uint8_t> encodeHandshakeResponse(const std::string& peerId,
                                             uint16_t screenWidth,
                                             uint16_t screenHeight);
bool decodeHandshakeResponse(const std::vector<uint8_t>& payload,
                             std::string& peerId,
                             uint16_t& screenWidth,
                             uint16_t& screenHeight);

std::vector<uint8_t> encodeHeartbeat(const std::string& peerId);
bool decodeHeartbeat(const std::vector<uint8_t>& payload, std::string& peerId);

std::vector<uint8_t> encodeMouseMove(uint32_t sequence, uint16_t x, uint16_t y);
// Accepts the legacy 4-byte format and the current 8-byte sequence format.
bool decodeMouseMove(const std::vector<uint8_t>& payload, uint16_t& x, uint16_t& y);
bool decodeMouseMove(const std::vector<uint8_t>& payload, uint32_t& sequence,
                     uint16_t& x, uint16_t& y);

std::vector<uint8_t> encodeMouseButton(MouseButtonCode button, MouseAction action);
bool decodeMouseButton(const std::vector<uint8_t>& payload,
                       MouseButtonCode& button,
                       MouseAction& action);

std::vector<uint8_t> encodeMouseWheel(WheelDirection direction, int16_t delta);
bool decodeMouseWheel(const std::vector<uint8_t>& payload,
                      WheelDirection& direction,
                      int16_t& delta);

std::vector<uint8_t> encodeKeyEvent(MessageType type,
                                    uint16_t vkCode,
                                    uint16_t scanCode,
                                    bool extendedKey);
bool decodeKeyEvent(const std::vector<uint8_t>& payload,
                    uint16_t& vkCode,
                    uint16_t& scanCode,
                    bool& extendedKey);

std::vector<uint8_t> encodeSwitchIn(uint16_t x, uint16_t y);
bool decodeSwitchIn(const std::vector<uint8_t>& payload, uint16_t& x, uint16_t& y);

// Payload is raw UTF-8 bytes; an empty payload decodes as empty text.
std::vector<uint8_t> encodeClipboardText(const std::string& utf8);
bool decodeClipboardText(const std::vector<uint8_t>& payload, std::string& utf8);

// Wire payload carries name/size/dir only; the local path never leaves the
// source machine.
std::vector<uint8_t> encodeFilesMeta(const std::vector<FileEntry>& files);
bool decodeFilesMeta(const std::vector<uint8_t>& payload, std::vector<FileEntry>& files);

// files=true encodes {"kind":"files"} (transfer actual file data over TCP);
// an empty payload is the legacy "send me your clipboard content" request.
std::vector<uint8_t> encodeClipboardRequest(bool files);
bool decodeClipboardRequest(const std::vector<uint8_t>& payload, bool& files);

std::vector<uint8_t> encodeSlaveHello(const std::string& hostname,
                                      uint16_t filePort,
                                      const std::string& presharedKey);
bool decodeSlaveHello(const std::vector<uint8_t>& payload,
                      std::string& hostname,
                      uint16_t& filePort,
                      std::string& presharedKey);

}  // namespace synccontrol
