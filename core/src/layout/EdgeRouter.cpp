#include "layout/EdgeRouter.h"

#include <algorithm>
#include <cmath>

namespace synccontrol {

EdgeHit detectEdge(int x, int y, int screenW, int screenH) {
    if (screenW <= 0 || screenH <= 0) {
        return {};
    }

    EdgeHit hit;
    if (x <= 0) {
        hit.edge = Edge::Left;
        hit.ratio = static_cast<float>(y) / static_cast<float>(screenH);
    } else if (x >= screenW - 1) {
        hit.edge = Edge::Right;
        hit.ratio = static_cast<float>(y) / static_cast<float>(screenH);
    } else if (y <= 0) {
        hit.edge = Edge::Top;
        hit.ratio = static_cast<float>(x) / static_cast<float>(screenW);
    } else if (y >= screenH - 1) {
        hit.edge = Edge::Bottom;
        hit.ratio = static_cast<float>(x) / static_cast<float>(screenW);
    } else {
        return {};
    }
    hit.atEdge = true;
    return hit;
}

bool lookupRoute(const ScreenRouting& routing, Edge edge, float ratio, EdgeSegment& out) {
    const std::vector<EdgeSegment>& segments =
        routing.segments[static_cast<size_t>(edge)];
    for (const EdgeSegment& segment : segments) {
        if (ratio >= segment.rangeStart - kRouteEpsilon &&
            ratio < segment.rangeEnd - kRouteEpsilon) {
            out = segment;
            return true;
        }
    }
    return false;
}

TargetPos calcTargetPos(const EdgeSegment& segment, float globalRatio,
                        int targetW, int targetH) {
    const float span = segment.rangeEnd - segment.rangeStart;
    float localRatio = 0.0f;
    if (span > kRouteEpsilon) {
        localRatio = (globalRatio - segment.rangeStart) / span;
    }
    localRatio = std::clamp(localRatio, 0.0f, 1.0f);

    const int maxX = std::max(targetW - 1, 0);
    const int maxY = std::max(targetH - 1, 0);
    const int scaledX = static_cast<int>(std::lround(localRatio * static_cast<float>(maxX)));
    const int scaledY = static_cast<int>(std::lround(localRatio * static_cast<float>(maxY)));

    TargetPos pos;
    switch (segment.toEdge) {
        case Edge::Left:
            pos.x = 0;
            pos.y = scaledY;
            break;
        case Edge::Right:
            pos.x = maxX;
            pos.y = scaledY;
            break;
        case Edge::Top:
            pos.x = scaledX;
            pos.y = 0;
            break;
        case Edge::Bottom:
            pos.x = scaledX;
            pos.y = maxY;
            break;
    }
    return pos;
}

}  // namespace synccontrol
