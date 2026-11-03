#include "layout/EdgeSwitchPolicy.h"

#include <algorithm>
#include <cmath>

namespace synccontrol {

namespace {

size_t edgeIndex(Edge edge) {
    switch (edge) {
        case Edge::Top: return 0;
        case Edge::Bottom: return 1;
        case Edge::Left: return 2;
        case Edge::Right: return 3;
    }
    return 4;
}

}  // namespace

void EdgeSwitchPolicy::setConfig(const Config& config) {
    if (config.mode != m_config.mode ||
        config.bumpWindowMs != m_config.bumpWindowMs ||
        config.bumpCount != m_config.bumpCount ||
        config.dwellMs != m_config.dwellMs) {
        m_config = config;
        resetState();
    }
}

void EdgeSwitchPolicy::reset() {
    resetState();
}

void EdgeSwitchPolicy::resetState() {
    m_bumpEdgeIndex = 4;
    m_bumpRatio = 0.0f;
    m_bumpCount = 0;
    m_lastBumpMs = 0;
    m_dwelling = false;
    m_dwellStartMs = 0;
}

EdgeSwitchPolicy::Verdict EdgeSwitchPolicy::onMove(bool atEdge, Edge edge, float ratio,
                                                   int innerDistance, uint64_t nowMs) {
    if (m_config.mode == Mode::Immediate) {
        // 无状态：到达边缘即切换。
        return atEdge ? Verdict::Switch : Verdict::Pending;
    }

    if (!atEdge) {
        // 离开边缘区：结束停留等待；bump 计数交给时间窗口控制。
        if (m_dwelling) {
            resetState();
        }
        return Verdict::Pending;
    }

    if (m_config.mode == Mode::Dwell) {
        const size_t index = edgeIndex(edge);
        if (!m_dwelling) {
            m_dwelling = true;
            m_dwellEdge = edge;
            m_dwellRatio = ratio;
            m_dwellStartMs = nowMs;
            return Verdict::Pending;
        }
        if (edgeIndex(m_dwellEdge) != index ||
            std::fabs(m_dwellRatio - ratio) > kRatioTolerance) {
            // 贴边滑动到另一边缘或明显不同位置：重新计时。
            m_dwellEdge = edge;
            m_dwellRatio = ratio;
            m_dwellStartMs = nowMs;
        }
        return Verdict::Pending;
    }

    // bump 模式：光标上一次位置在边缘内侧超过臂距，当前位置到达边缘才算一次碰撞。
    // 贴边平行滑动（上一次位置距边 ≤ 臂距）不产生新碰撞。
    if (innerDistance <= kBumpArmPixels) {
        return Verdict::Pending;
    }

    const size_t index = edgeIndex(edge);
    if (m_bumpEdgeIndex != index ||
        std::fabs(m_bumpRatio - ratio) > kRatioTolerance ||
        (m_bumpCount > 0 && nowMs - m_lastBumpMs > m_config.bumpWindowMs)) {
        m_bumpEdgeIndex = static_cast<uint32_t>(index);
        m_bumpRatio = ratio;
        m_bumpCount = 0;
    }
    m_lastBumpMs = nowMs;
    ++m_bumpCount;
    if (m_bumpCount >= m_config.bumpCount) {
        m_bumpCount = 0;
        m_bumpEdgeIndex = 4;
        return Verdict::Switch;
    }
    return Verdict::Pending;
}

EdgeSwitchPolicy::Verdict EdgeSwitchPolicy::onDwellTimeout(Edge edge, uint64_t nowMs) {
    if (m_config.mode != Mode::Dwell) {
        return Verdict::Pending;
    }
    // 定时器是周期性的：即使贴边滑动重置过计时，只要仍贴在原边缘就继续起算，
    // 保证静止贴边的光标最终能完成切换。
    if (!m_dwelling) {
        m_dwelling = true;
        m_dwellEdge = edge;
        m_dwellStartMs = nowMs;
        return Verdict::Pending;
    }
    if (edgeIndex(m_dwellEdge) != edgeIndex(edge) ||
        nowMs - m_dwellStartMs < m_config.dwellMs) {
        m_dwellEdge = edge;
        m_dwellStartMs = nowMs;
        return Verdict::Pending;
    }
    resetState();
    return Verdict::Switch;
}

}  // namespace synccontrol
