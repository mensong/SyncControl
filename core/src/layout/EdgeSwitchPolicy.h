#pragma once

#include <cstdint>

#include "layout/ScreenLayout.h"

namespace synccontrol {

// 鼠标与屏幕边缘的"特征动作"判定状态机。
// 输入是每次鼠标事件的 (边缘, 位置, 时间戳)，输出是否允许切换。
// 与切换动作解耦：本地→远程与远程→远程/远程→本地共用同一实例。
class EdgeSwitchPolicy {
public:
    enum class Mode {
        Immediate,  // 碰边立即切换
        Bump,       // 时间窗口内同一边缘累计碰撞次数达到阈值才切换
        Dwell,      // 光标贴住边缘持续一段时间才切换
    };

    struct Config {
        Mode mode = Mode::Immediate;
        uint32_t bumpWindowMs = 1000;
        uint32_t bumpCount = 2;
        uint32_t dwellMs = 500;
    };

    // 由内向外冲击的判定臂距：光标上一次位置在边缘内侧超过该值，
    // 当前位置到达边缘才算一次碰撞。
    static constexpr int kBumpArmPixels = 8;
    // 同一边缘两次碰撞位置允许的相对偏移（0-1）。
    static constexpr float kRatioTolerance = 0.1f;

    enum class Verdict {
        Pending,   // 条件进行中，事件照常放行
        Switch,    // 条件满足，执行切换
    };

    EdgeSwitchPolicy() = default;
    explicit EdgeSwitchPolicy(const Config& config) : m_config(config) {}

    void setConfig(const Config& config);
    const Config& config() const { return m_config; }
    void reset();

    // 每次鼠标移动事件调用。atEdge 为 false 表示光标已离开边缘区，
    // 会重置停留状态。innerDistance 是光标当前位置到该边缘的距离，
    // 上一次位置距边 ≤ 臂距视为贴边滑动、不计碰撞。bump 到达阈值返回 Switch；
    // dwell 模式下返回 Pending，由定时器到期后再判定。
    Verdict onMove(bool atEdge, Edge edge, float ratio, int innerDistance,
                   uint64_t nowMs);

    // dwell 模式下光标持续贴在 edge 上的判定，由定时器到期时调用。
    // 仍贴在原边缘返回 Switch，否则返回 Pending 并重置状态。
    Verdict onDwellTimeout(Edge edge, uint64_t nowMs);

    // dwell 模式下当前是否处于贴边等待状态。
    bool dwelling() const { return m_dwelling; }
    Edge dwellEdge() const { return m_dwellEdge; }

private:
    void resetState();

    Config m_config;
    // bump 状态
    uint32_t m_bumpEdgeIndex = 4;  // 4 = 无
    float m_bumpRatio = 0.0f;
    uint32_t m_bumpCount = 0;
    uint64_t m_lastBumpMs = 0;
    // dwell 状态
    bool m_dwelling = false;
    Edge m_dwellEdge = Edge::Top;
    float m_dwellRatio = 0.0f;
    uint64_t m_dwellStartMs = 0;
};

}  // namespace synccontrol
