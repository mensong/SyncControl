#pragma once

#include <cstdint>

#include "net/Protocol.h"

namespace synccontrol {

class InputInject {
public:
    bool mouseMove(uint16_t x, uint16_t y) const;
    bool mouseButton(MouseButtonCode button, MouseAction action) const;
    bool mouseWheel(WheelDirection direction, int16_t delta) const;
    bool keyEvent(bool down, uint16_t vkCode, uint16_t scanCode, bool extendedKey) const;
    bool switchIn(uint16_t x, uint16_t y) const;
    void switchOut() const;
};

}  // namespace synccontrol
