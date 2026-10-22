#include "input/InputInject.h"

#include <Windows.h>

namespace synccontrol {
namespace {

UINT absoluteCoordinate(uint16_t coordinate, int screenExtent) {
    if (screenExtent <= 0) {
        return 0;
    }
    const int clamped = coordinate < static_cast<uint16_t>(screenExtent)
                            ? static_cast<int>(coordinate)
                            : screenExtent - 1;
    return static_cast<UINT>(static_cast<UINT64>(clamped) * 65535U /
                             static_cast<UINT64>(screenExtent));
}

bool sendMouseInput(DWORD flags, DWORD mouseData) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flags;
    input.mi.mouseData = mouseData;
    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

DWORD buttonDownFlag(MouseButtonCode button) {
    switch (button) {
        case MouseButtonCode::Left:
            return MOUSEEVENTF_LEFTDOWN;
        case MouseButtonCode::Right:
            return MOUSEEVENTF_RIGHTDOWN;
        case MouseButtonCode::Middle:
            return MOUSEEVENTF_MIDDLEDOWN;
        case MouseButtonCode::X1:
        case MouseButtonCode::X2:
            return MOUSEEVENTF_XDOWN;
    }
    return 0;
}

DWORD buttonUpFlag(MouseButtonCode button) {
    switch (button) {
        case MouseButtonCode::Left:
            return MOUSEEVENTF_LEFTUP;
        case MouseButtonCode::Right:
            return MOUSEEVENTF_RIGHTUP;
        case MouseButtonCode::Middle:
            return MOUSEEVENTF_MIDDLEUP;
        case MouseButtonCode::X1:
        case MouseButtonCode::X2:
            return MOUSEEVENTF_XUP;
    }
    return 0;
}

}  // namespace

bool InputInject::mouseMove(uint16_t x, uint16_t y) const {
    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    input.mi.dx = static_cast<LONG>(absoluteCoordinate(x, screenW));
    input.mi.dy = static_cast<LONG>(absoluteCoordinate(y, screenH));
    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

bool InputInject::mouseButton(MouseButtonCode button, MouseAction action) const {
    const DWORD flag = action == MouseAction::Down ? buttonDownFlag(button)
                                                   : buttonUpFlag(button);
    if (flag == 0) {
        return false;
    }

    DWORD mouseData = 0;
    if (flag == MOUSEEVENTF_XDOWN || flag == MOUSEEVENTF_XUP) {
        mouseData = button == MouseButtonCode::X2 ? XBUTTON2 : XBUTTON1;
    }
    return sendMouseInput(flag, mouseData);
}

bool InputInject::mouseWheel(WheelDirection direction, int16_t delta) const {
    const DWORD flag = direction == WheelDirection::Vertical ? MOUSEEVENTF_WHEEL
                                                             : MOUSEEVENTF_HWHEEL;
    return sendMouseInput(flag, static_cast<DWORD>(delta));
}

bool InputInject::keyEvent(bool down, uint16_t vkCode, uint16_t scanCode,
                           bool extendedKey) const {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(vkCode);
    input.ki.wScan = static_cast<WORD>(scanCode);
    input.ki.dwFlags = extendedKey ? KEYEVENTF_EXTENDEDKEY : 0;
    if (!down) {
        input.ki.dwFlags |= KEYEVENTF_KEYUP;
    }
    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

bool InputInject::switchIn(uint16_t x, uint16_t y) const {
    return mouseMove(x, y);
}

void InputInject::switchOut() const {}

}  // namespace synccontrol
