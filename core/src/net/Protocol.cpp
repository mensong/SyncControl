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

void putU32(std::vector<uint8_t>& output, uint32_t value) {
    output.push_back(static_cast<uint8_t>(value & 0xFF));
    output.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    output.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    output.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

uint32_t getU32(const uint8_t* data, size_t offset) {
    return static_cast<uint32_t>(data[offset]) |
           (static_cast<uint32_t>(data[offset + 1]) << 8) |
           (static_cast<uint32_t>(data[offset + 2]) << 16) |
           (static_cast<uint32_t>(data[offset + 3]) << 24);
}

void appendBlob16(std::vector<uint8_t>& output, const uint8_t* data,
                  uint16_t bytes) {
    putU16(output, bytes);
    if (bytes > 0) {
        output.insert(output.end(), data, data + bytes);
    }
}

bool readBlob16(const std::vector<uint8_t>& payload, size_t& offset,
                const uint8_t*& data, uint16_t& bytes) {
    if (offset + 2 > payload.size()) {
        return false;
    }
    bytes = getU16(payload.data(), offset);
    offset += 2;
    if (offset + bytes > payload.size()) {
        return false;
    }
    data = payload.data() + offset;
    offset += bytes;
    return true;
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

std::vector<uint8_t> encodeHeartbeatPing(const std::string& peerId,
                                         const std::string& keyHash) {
    const json payload = {
        {"id", peerId},
        {"keyHash", keyHash},
    };
    const std::string text = payload.dump();
    return std::vector<uint8_t>(text.begin(), text.end());
}

bool decodeHeartbeatPing(const std::vector<uint8_t>& payload,
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

std::vector<uint8_t> encodeClipboardImageMeta(const ClipboardImageMeta& meta) {
    if (meta.transferId.empty()) {
        return {};
    }
    const json payload = {
        {"transferId", meta.transferId},
        {"width", meta.width},
        {"height", meta.height},
        {"bytes", meta.totalBytes},
        {"origin", meta.originPeerId},
        {"originSequence", meta.originClipboardSequence},
    };
    const std::string text = payload.dump();
    return std::vector<uint8_t>(text.begin(), text.end());
}

bool decodeClipboardImageMeta(const std::vector<uint8_t>& payload,
                              ClipboardImageMeta& meta) {
    meta = {};
    try {
        const json value = json::parse(payload.begin(), payload.end());
        meta.transferId = value.at("transferId").get<std::string>();
        meta.width = value.value("width", 0U);
        meta.height = value.value("height", 0U);
        meta.totalBytes = value.value("bytes", 0U);
        meta.originPeerId = value.value("origin", std::string());
        const uint64_t sequence = value.value("originSequence", 0ULL);
        meta.originClipboardSequence = static_cast<uint32_t>(sequence);
    } catch (...) {
        return false;
    }
    return !meta.transferId.empty() && meta.width > 0 && meta.height > 0 &&
           meta.totalBytes > 0 && meta.totalBytes <= kMaxClipboardImageBytes;
}

std::vector<uint8_t> encodeClipboardImageRequest(const std::string& transferId) {
    if (transferId.empty()) {
        return {};
    }
    const json payload = {{"transferId", transferId}};
    const std::string text = payload.dump();
    return std::vector<uint8_t>(text.begin(), text.end());
}

bool decodeClipboardImageRequest(const std::vector<uint8_t>& payload,
                                 std::string& transferId) {
    transferId.clear();
    try {
        const json value = json::parse(payload.begin(), payload.end());
        transferId = value.at("transferId").get<std::string>();
    } catch (...) {
        return false;
    }
    return !transferId.empty() && transferId.size() <= 128;
}

std::vector<uint8_t> encodeClipboardRichMeta(const ClipboardRichMeta& meta) {
    if (meta.transferId.empty()) {
        return {};
    }
    const json payload = {
        {"transferId", meta.transferId},
        {"kind", meta.kind == ClipboardRichKind::Rtf ? "rtf" : "html"},
        {"richBytes", meta.richBytes},
        {"textBytes", meta.companionTextBytes},
        {"origin", meta.originPeerId},
        {"originSequence", meta.originClipboardSequence},
    };
    const std::string text = payload.dump();
    return std::vector<uint8_t>(text.begin(), text.end());
}

bool decodeClipboardRichMeta(const std::vector<uint8_t>& payload,
                             ClipboardRichMeta& meta) {
    meta = {};
    try {
        const json value = json::parse(payload.begin(), payload.end());
        meta.transferId = value.at("transferId").get<std::string>();
        meta.kind = value.value("kind", std::string()) == "rtf"
                        ? ClipboardRichKind::Rtf
                        : ClipboardRichKind::Html;
        meta.richBytes = value.value("richBytes", 0ULL);
        meta.companionTextBytes = value.value("textBytes", 0ULL);
        meta.originPeerId = value.value("origin", std::string());
        const uint64_t sequence = value.value("originSequence", 0ULL);
        meta.originClipboardSequence = static_cast<uint32_t>(sequence);
    } catch (...) {
        return false;
    }
    return !meta.transferId.empty() && meta.richBytes > 0 &&
           meta.richBytes <= kMaxClipboardRichBytes;
}

std::vector<uint8_t> encodeClipboardRichRequest(const std::string& transferId) {
    if (transferId.empty()) {
        return {};
    }
    const json payload = {{"transferId", transferId}};
    const std::string text = payload.dump();
    return std::vector<uint8_t>(text.begin(), text.end());
}

bool decodeClipboardRichRequest(const std::vector<uint8_t>& payload,
                                std::string& transferId) {
    transferId.clear();
    try {
        const json value = json::parse(payload.begin(), payload.end());
        transferId = value.at("transferId").get<std::string>();
    } catch (...) {
        return false;
    }
    return !transferId.empty() && transferId.size() <= 128;
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
