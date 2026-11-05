#pragma once

#include <cstdint>

#include "net/Protocol.h"

namespace synccontrol {

class InputInject {
public:
    // Virtual so the helper-forwarding subclass can override and fall back to
    // these implementations when the SYSTEM session helper is unavailable.
    virtual ~InputInject() = default;
    virtual bool mouseMove(uint16_t x, uint16_t y) const;
    virtual bool mouseButton(MouseButtonCode button, MouseAction action) const;
    virtual bool mouseWheel(WheelDirection direction, int16_t delta) const;
    virtual bool keyEvent(bool down, uint16_t vkCode, uint16_t scanCode, bool extendedKey) const;
    virtual bool switchIn(uint16_t x, uint16_t y) const;
    virtual void switchOut() const;
};

}  // namespace synccontrol
