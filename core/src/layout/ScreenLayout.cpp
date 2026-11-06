#include "layout/ScreenLayout.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>

namespace synccontrol {
namespace {

constexpr float kRangeEpsilon = 1e-4f;

}  // namespace

bool edgeFromString(const std::string& name, Edge& out) {
    std::string value;
    value.reserve(name.size());
    for (char c : name) {
        value.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    if (value == "top") {
        out = Edge::Top;
    } else if (value == "bottom") {
        out = Edge::Bottom;
    } else if (value == "left") {
        out = Edge::Left;
    } else if (value == "right") {
        out = Edge::Right;
    } else {
        return false;
    }
    return true;
}

const char* edgeToString(Edge edge) {
    switch (edge) {
        case Edge::Top:
            return "top";
        case Edge::Bottom:
            return "bottom";
        case Edge::Left:
            return "left";
        case Edge::Right:
            return "right";
    }
    return "top";
}

ScreenLayout::ScreenLayout(const ScreenLayout& other) {
    std::lock_guard<std::mutex> lock(other.m_mutex);
    m_nodes = other.m_nodes;
    m_routings = other.m_routings;
}

ScreenLayout& ScreenLayout::operator=(const ScreenLayout& other) {
    if (this == &other) {
        return *this;
    }
    std::vector<ScreenNode> nodes;
    std::vector<ScreenRouting> routings;
    {
        std::lock_guard<std::mutex> lock(other.m_mutex);
        nodes = other.m_nodes;
        routings = other.m_routings;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_nodes = std::move(nodes);
    m_routings = std::move(routings);
    return *this;
}

bool ScreenLayout::loadFromJson(const nlohmann::json& layout) {
    if (!layout.is_object()) {
        return false;
    }

    const auto screensIt = layout.find("screens");
    if (screensIt == layout.end() || !screensIt->is_array() || screensIt->empty()) {
        return false;
    }

    std::vector<ScreenNode> nodes;
    std::set<std::string> ids;
    for (const auto& entry : *screensIt) {
        if (!entry.is_object()) {
            return false;
        }
        const auto idIt = entry.find("id");
        const auto hostnameIt = entry.find("hostname");
        const auto ipIt = entry.find("ip");
        const auto portIt = entry.find("port");
        const auto widthIt = entry.find("screenW");
        const auto heightIt = entry.find("screenH");
        if ((idIt != entry.end() && !idIt->is_string()) ||
            (hostnameIt != entry.end() && !hostnameIt->is_string()) ||
            (ipIt != entry.end() && !ipIt->is_string()) ||
            (portIt != entry.end() && !portIt->is_number()) ||
            (widthIt != entry.end() && !widthIt->is_number()) ||
            (heightIt != entry.end() && !heightIt->is_number())) {
            return false;
        }
        ScreenNode node;
        node.id = entry.value("id", std::string());
        node.hostname = entry.value("hostname", std::string());
        node.ip = entry.value("ip", std::string());
        node.port = static_cast<uint16_t>(entry.value("port", 19881));
        node.screenW = entry.value("screenW", 0);
        node.screenH = entry.value("screenH", 0);
        if (node.id.empty() || node.screenW < 0 || node.screenH < 0) {
            return false;
        }
        if (!ids.insert(node.id).second) {
            return false;
        }
        nodes.push_back(std::move(node));
    }

    std::vector<ScreenRouting> routings;
    const auto routingIt = layout.find("routing");
    if (routingIt != layout.end() && routingIt->is_object()) {
        for (auto item = routingIt->begin(); item != routingIt->end(); ++item) {
            if (!item->is_object() || ids.find(item.key()) == ids.end()) {
                return false;
            }
            ScreenRouting routing;
            routing.screenId = item.key();
            for (int edgeIndex = 0; edgeIndex < 4; ++edgeIndex) {
                const auto segmentsIt = item->find(edgeToString(static_cast<Edge>(edgeIndex)));
                if (segmentsIt == item->end()) {
                    continue;
                }
                if (!segmentsIt->is_array()) {
                    return false;
                }
                for (const auto& entry : *segmentsIt) {
                    if (!entry.is_object()) {
                        return false;
                    }
                    const auto startIt = entry.find("rangeStart");
                    const auto endIt = entry.find("rangeEnd");
                    const auto toIdIt = entry.find("toId");
                    const auto toEdgeIt = entry.find("toEdge");
                    if ((startIt != entry.end() && !startIt->is_number()) ||
                        (endIt != entry.end() && !endIt->is_number()) ||
                        (toIdIt != entry.end() && !toIdIt->is_string()) ||
                        (toEdgeIt != entry.end() && !toEdgeIt->is_string())) {
                        return false;
                    }
                    EdgeSegment segment;
                    segment.fromEdge = static_cast<Edge>(edgeIndex);
                    segment.rangeStart = entry.value("rangeStart", 0.0f);
                    segment.rangeEnd = entry.value("rangeEnd", 0.0f);
                    segment.toId = entry.value("toId", std::string());
                    if (!edgeFromString(entry.value("toEdge", std::string()), segment.toEdge)) {
                        return false;
                    }
                    if (segment.toId.empty() || ids.find(segment.toId) == ids.end()) {
                        return false;
                    }
                    if (segment.rangeStart < -kRangeEpsilon ||
                        segment.rangeEnd > 1.0f + kRangeEpsilon ||
                        segment.rangeStart > segment.rangeEnd + kRangeEpsilon) {
                        return false;
                    }
                    segment.rangeStart = std::max(0.0f, segment.rangeStart);
                    segment.rangeEnd = std::min(1.0f, segment.rangeEnd);
                    routing.segments[edgeIndex].push_back(std::move(segment));
                }
                auto& segments = routing.segments[edgeIndex];
                std::sort(segments.begin(), segments.end(),
                          [](const EdgeSegment& left, const EdgeSegment& right) {
                              return left.rangeStart < right.rangeStart;
                          });
            }
            routings.push_back(std::move(routing));
        }
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_nodes = std::move(nodes);
    m_routings = std::move(routings);
    return true;
}

bool ScreenLayout::loadFromFile(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        return false;
    }
    nlohmann::json layout;
    try {
        file >> layout;
    } catch (const nlohmann::json::parse_error&) {
        return false;
    }
    return loadFromJson(layout);
}

void ScreenLayout::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_nodes.clear();
    m_routings.clear();
}

std::vector<ScreenNode> ScreenLayout::nodes() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_nodes;
}

bool ScreenLayout::findNode(const std::string& id, ScreenNode& out) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const ScreenNode& node : m_nodes) {
        if (node.id == id) {
            out = node;
            return true;
        }
    }
    return false;
}

bool ScreenLayout::findRouting(const std::string& id, ScreenRouting& out) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const ScreenRouting& routing : m_routings) {
        if (routing.screenId == id) {
            out = routing;
            return true;
        }
    }
    return false;
}

bool ScreenLayout::empty() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_nodes.empty();
}

}  // namespace synccontrol
