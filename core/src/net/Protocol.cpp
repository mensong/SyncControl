#include "net/Protocol.h"

#include <json.hpp>

#include "core/Security.h"

namespace synccontrol {
namespace {

using nlohmann::json;

void putU16(std::vector<uint8_t>& output, uint16_t value) {
    output.push_back(static_cast<uint8_t>(value & 0xFF));
    output.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

uint16_t getU16(const uint8_t* data, size_t offset) {
    return static_cast<uint16_t>(data[offset]) |
           static_cast<uint16_t>(static_cast<uint16_t>(data[offset + 1]) << 8);
}

void appendU8(std::vector<uint8_t>& output, uint8_t value) {
    output.push_back(value);
}

void appendI16(std::vector<uint8_t>& output, int16_t value) {
    putU16(output, static_cast<uint16_t>(value));
}

int16_t getI16(const uint8_t* data, size_t offset) {
    return static_cast<int16_t>(getU16(data, offset));
}

}  // namespace

std::vector<uint8_t> serializeMessage(const Message& message) {
    std::vector<uint8_t> output;
    output.reserve(message.payload.size() + 4);
    putU16(output, static_cast<uint16_t>(message.type));
    putU16(output, static_cast<uint16_t>(message.payload.size()));
    output.insert(output.end(), message.payload.begin(), message.payload.end());
    return output;
}

bool decodeMessage(const uint8_t* data, size_t size, size_t& offset, Message& message) {
    if (offset + 4 > size) {
        return false;
    }

    const uint16_t type = getU16(data, offset);
    const uint16_t length = getU16(data, offset + 2);
    const size_t payloadOffset = offset + 4;
    if (payloadOffset + length > size) {
        return false;
    }

    message.type = static_cast<MessageType>(type);
    message.payload.assign(data + payloadOffset, data + payloadOffset + length);
    offset = payloadOffset + length;
    return true;
}

Channel routeChannel(MessageType type) {
    if (type == MessageType::MouseMove) {
        return Channel::UdpRaw;
    }
    if (type == MessageType::ClipboardFileData) {
        return Channel::Tcp;
    }
    return Channel::Kcp;
}

std::vector<uint8_t> encodeHandshakeRequest(const std::string& peerId,
                                            const std::string& presharedKey) {
    const json payload = {
        {"id", peerId},
        {"keyHash", sha256Hex(presharedKey)},
    };
    const std::string text = payload.dump();
    return std::vector<uint8_t>(text.begin(), text.end());
}

bool decodeHandshakeRequest(const std::vector<uint8_t>& payload,
                            std::string& peerId,
                            std::string& keyHash) {
    try {
        const json value = json::parse(payload.begin(), payload.end());
        peerId = value.at("id").get<std::string>();
        keyHash = value.at("keyHash").get<std::string>();
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<uint8_t> encodeHandshakeResponse(const std::string& peerId,
                                             uint16_t screenW,
                                             uint16_t screenH) {
    const json payload = {
        {"id", peerId},
        {"screenW", screenW},
        {"screenH", screenH},
    };
    const std::string text = payload.dump();
    return std::vector<uint8_t>(text.begin(), text.end());
}

bool decodeHandshakeResponse(const std::vector<uint8_t>& payload,
                             std::string& peerId,
                             uint16_t& screenW,
                             uint16_t& screenH) {
    try {
        const json value = json::parse(payload.begin(), payload.end());
        peerId = value.at("id").get<std::string>();
        screenW = value.at("screenW").get<uint16_t>();
        screenH = value.at("screenH").get<uint16_t>();
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<uint8_t> encodeHeartbeat(const std::string& peerId) {
    const json payload = {{"id", peerId}};
    const std::string text = payload.dump();
    return std::vector<uint8_t>(text.begin(), text.end());
}

bool decodeHeartbeat(const std::vector<uint8_t>& payload, std::string& peerId) {
    try {
        const json value = json::parse(payload.begin(), payload.end());
        peerId = value.at("id").get<std::string>();
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<uint8_t> encodeMouseMove(uint32_t sequence, uint16_t x, uint16_t y) {
    std::vector<uint8_t> payload;
    payload.push_back(static_cast<uint8_t>(sequence & 0xFF));
    payload.push_back(static_cast<uint8_t>((sequence >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>((sequence >> 16) & 0xFF));
    payload.push_back(static_cast<uint8_t>((sequence >> 24) & 0xFF));
    putU16(payload, x);
    putU16(payload, y);
    return payload;
}

bool decodeMouseMove(const std::vector<uint8_t>& payload, uint16_t& x, uint16_t& y) {
    uint32_t sequence = 0;
    return decodeMouseMove(payload, sequence, x, y);
}

bool decodeMouseMove(const std::vector<uint8_t>& payload, uint32_t& sequence,
                     uint16_t& x, uint16_t& y) {
    if (payload.size() == 4) {
        sequence = 0;
        x = getU16(payload.data(), 0);
        y = getU16(payload.data(), 2);
        return true;
    }
    if (payload.size() != 8) {
        return false;
    }
    sequence = static_cast<uint32_t>(payload[0]) |
               (static_cast<uint32_t>(payload[1]) << 8) |
               (static_cast<uint32_t>(payload[2]) << 16) |
               (static_cast<uint32_t>(payload[3]) << 24);
    x = getU16(payload.data(), 4);
    y = getU16(payload.data(), 6);
    return true;
}

std::vector<uint8_t> encodeMouseButton(MouseButtonCode button, MouseAction action) {
    std::vector<uint8_t> payload;
    appendU8(payload, static_cast<uint8_t>(button));
    appendU8(payload, static_cast<uint8_t>(action));
    return payload;
}

bool decodeMouseButton(const std::vector<uint8_t>& payload,
                       MouseButtonCode& button,
                       MouseAction& action) {
    if (payload.size() != 2) {
        return false;
    }
    button = static_cast<MouseButtonCode>(payload[0]);
    action = static_cast<MouseAction>(payload[1]);
    return button <= MouseButtonCode::X2 && action <= MouseAction::Up;
}

std::vector<uint8_t> encodeMouseWheel(WheelDirection direction, int16_t delta) {
    std::vector<uint8_t> payload;
    appendU8(payload, static_cast<uint8_t>(direction));
    appendI16(payload, delta);
    return payload;
}

bool decodeMouseWheel(const std::vector<uint8_t>& payload,
                      WheelDirection& direction,
                      int16_t& delta) {
    if (payload.size() != 3) {
        return false;
    }
    direction = static_cast<WheelDirection>(payload[0]);
    delta = getI16(payload.data(), 1);
    return direction <= WheelDirection::Horizontal;
}

std::vector<uint8_t> encodeKeyEvent(MessageType type,
                                    uint16_t vkCode,
                                    uint16_t scanCode,
                                    bool extendedKey) {
    if (type != MessageType::KeyDown && type != MessageType::KeyUp) {
        return {};
    }

    std::vector<uint8_t> payload;
    putU16(payload, vkCode);
    putU16(payload, scanCode);
    appendU8(payload, extendedKey ? 1 : 0);
    return payload;
}

bool decodeKeyEvent(const std::vector<uint8_t>& payload,
                    uint16_t& vkCode,
                    uint16_t& scanCode,
                    bool& extendedKey) {
    if (payload.size() != 5) {
        return false;
    }
    vkCode = getU16(payload.data(), 0);
    scanCode = getU16(payload.data(), 2);
    extendedKey = payload[4] != 0;
    return true;
}

std::vector<uint8_t> encodeSwitchIn(uint16_t x, uint16_t y) {
    std::vector<uint8_t> payload;
    putU16(payload, x);
    putU16(payload, y);
    return payload;
}

bool decodeSwitchIn(const std::vector<uint8_t>& payload, uint16_t& x, uint16_t& y) {
    if (payload.size() != 4) {
        return false;
    }
    x = getU16(payload.data(), 0);
    y = getU16(payload.data(), 2);
    return true;
}

std::vector<uint8_t> encodeClipboardText(const std::string& utf8) {
    return std::vector<uint8_t>(utf8.begin(), utf8.end());
}

bool decodeClipboardText(const std::vector<uint8_t>& payload, std::string& utf8) {
    utf8.assign(payload.begin(), payload.end());
    return true;
}

std::vector<uint8_t> encodeFilesMeta(const std::vector<FileEntry>& files,
                                     const std::string& originPeerId,
                                     uint32_t originClipboardSequence) {
    json array = json::array();
    for (const FileEntry& entry : files) {
        array.push_back({
            {"name", entry.name},
            {"size", entry.size},
            {"dir", entry.isDir},
        });
    }
    const json payload = {
        {"files", array},
        {"origin", originPeerId},
        {"originSequence", originClipboardSequence},
    };
    const std::string text = payload.dump();
    return std::vector<uint8_t>(text.begin(), text.end());
}

bool decodeFilesMeta(const std::vector<uint8_t>& payload,
                     std::vector<FileEntry>& files,
                     std::string& originPeerId,
                     uint32_t& originClipboardSequence) {
    files.clear();
    originPeerId.clear();
    originClipboardSequence = 0;
    try {
        const json value = json::parse(payload.begin(), payload.end());
        originPeerId = value.value("origin", std::string());
        const uint64_t sequence = value.value("originSequence", 0ULL);
        originClipboardSequence = static_cast<uint32_t>(sequence);
        for (const json& item : value.at("files")) {
            FileEntry entry;
            entry.name = item.at("name").get<std::string>();
            entry.size = item.value("size", 0ULL);
            entry.isDir = item.value("dir", false);
            if (entry.name.empty()) {
                return false;
            }
            files.push_back(std::move(entry));
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<uint8_t> encodeClipboardRequest(bool files) {
    if (!files) {
        return {};
    }
    const json payload = {{"kind", "files"}};
    const std::string text = payload.dump();
    return std::vector<uint8_t>(text.begin(), text.end());
}

bool decodeClipboardRequest(const std::vector<uint8_t>& payload, bool& files) {
    files = false;
    if (payload.empty()) {
        return true;  // legacy text request
    }
    try {
        const json value = json::parse(payload.begin(), payload.end());
        files = value.value("kind", std::string()) == "files";
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<uint8_t> encodeSlaveHello(const std::string& hostname,
                                      uint16_t filePort,
                                      const std::string& presharedKey) {
    const json payload = {
        {"id", hostname},
        {"filePort", filePort},
        {"key", presharedKey},
    };
    const std::string text = payload.dump();
    return std::vector<uint8_t>(text.begin(), text.end());
}

bool decodeSlaveHello(const std::vector<uint8_t>& payload,
                      std::string& hostname,
                      uint16_t& filePort,
                      std::string& presharedKey) {
    try {
        const json value = json::parse(payload.begin(), payload.end());
        hostname = value.at("id").get<std::string>();
        filePort = value.at("filePort").get<uint16_t>();
        presharedKey = value.at("key").get<std::string>();
        return !hostname.empty();
    } catch (...) {
        return false;
    }
}

}  // namespace synccontrol
