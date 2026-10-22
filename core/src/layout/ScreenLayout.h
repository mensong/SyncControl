#pragma once

#include <mutex>
#include <string>
#include <vector>

#include <json.hpp>

namespace synccontrol {

// Segment arrays are indexed by this enum: Top=0, Bottom=1, Left=2, Right=3.
enum class Edge {
    Top,
    Bottom,
    Left,
    Right,
};

struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct ScreenNode {
    std::string id;
    std::string hostname;
    std::string ip;
    uint16_t port = 19881;
    int screenW = 0;
    int screenH = 0;
    Rect layoutRect{};
};

struct EdgeSegment {
    Edge fromEdge = Edge::Top;
    float rangeStart = 0.0f;
    float rangeEnd = 0.0f;
    std::string toId;
    Edge toEdge = Edge::Top;
};

struct ScreenRouting {
    std::string screenId;
    std::vector<EdgeSegment> segments[4];
};

bool edgeFromString(const std::string& name, Edge& out);
const char* edgeToString(Edge edge);

class ScreenLayout {
public:
    ScreenLayout() = default;
    ScreenLayout(const ScreenLayout& other);
    ScreenLayout& operator=(const ScreenLayout& other);

    // Replaces the whole layout only when the JSON is fully valid, so a bad
    // hot update never leaves a half-parsed routing table behind.
    bool loadFromJson(const nlohmann::json& layout);
    nlohmann::json toJson() const;
    bool loadFromFile(const std::string& path);
    bool saveToFile(const std::string& path) const;
    void clear();

    std::vector<ScreenNode> nodes() const;
    bool findNode(const std::string& id, ScreenNode& out) const;
    bool findRouting(const std::string& id, ScreenRouting& out) const;
    bool empty() const;

private:
    mutable std::mutex m_mutex;
    std::vector<ScreenNode> m_nodes;
    std::vector<ScreenRouting> m_routings;
};

}  // namespace synccontrol
