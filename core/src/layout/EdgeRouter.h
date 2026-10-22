#pragma once

#include "layout/ScreenLayout.h"

namespace synccontrol {

struct EdgeHit {
    bool atEdge = false;
    Edge edge = Edge::Top;
    float ratio = 0.0f;
};

struct TargetPos {
    int x = 0;
    int y = 0;
};

constexpr float kRouteEpsilon = 1e-4f;

// Detects whether a point touches one of the screen edges. Horizontal edges
// report mouseX/screenW as ratio, vertical edges report mouseY/screenH.
EdgeHit detectEdge(int x, int y, int screenW, int screenH);

// Returns the segment covering [rangeStart, rangeEnd) with epsilon tolerance.
bool lookupRoute(const ScreenRouting& routing, Edge edge, float ratio, EdgeSegment& out);

// Maps a position along the source edge onto the target screen. The result is
// clamped to the target screen so the entry point stays inside [0, size-1].
TargetPos calcTargetPos(const EdgeSegment& segment, float globalRatio,
                        int targetW, int targetH);

}  // namespace synccontrol
