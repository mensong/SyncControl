#include "input/InputCapture.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/Logger.h"
namespace synccontrol {
namespace {

constexpr wchar_t kWindowClassName[] = L"SyncControlInputCaptureWindow";
constexpr int kReturnCursorInset = 32;
constexpr int kRemoteEntryInset = 8;
constexpr int kHookTargetMicroseconds = 100;
constexpr USHORT kGenericDesktopUsagePage = 1;
constexpr USHORT kMouseUsage = 2;
constexpr uint64_t kAbsoluteResetMs = 200;
constexpr int kCursorCoverAlpha = 1;

HCURSOR createTransparentCursor() {
    const int width = GetSystemMetrics(SM_CXCURSOR);
    const int height = GetSystemMetrics(SM_CYCURSOR);
    if (width <= 0 || height <= 0) {
        return nullptr;
    }

    const size_t stride = ((static_cast<size_t>(width) + 15) / 16) * 2;
    std::vector<BYTE> andMask(stride * static_cast<size_t>(height), 0xFF);
    std::vector<BYTE> xorMask(stride * static_cast<size_t>(height), 0x00);
    return CreateCursor(GetModuleHandleW(nullptr), 0, 0, width, height,
                        andMask.data(), xorMask.data());
}

const char* edgeName(Edge edge) {
    switch (edge) {
        case Edge::Left: return "left";
        case Edge::Right: return "right";
        case Edge::Top: return "top";
        case Edge::Bottom: return "bottom";
    }
    return "unknown";
}

LONG clampToRange(int value, int maximum) {
    return static_cast<LONG>(std::clamp(value, 0, maximum));
}

}  // namespace

InputCapture* InputCapture::s_instance = nullptr;

bool InputCapture::start(const Callbacks& callbacks) {
    if (m_mouseHook != nullptr || m_keyboardHook != nullptr || m_window != nullptr) {
        return false;
    }

    s_instance = this;
    m_callbacks = callbacks;
    m_focus = Focus::Local;
    m_returnHotkeyCtrlDown = false;
    m_returnHotkeyAltDown = false;
    m_systemCursorHidden = false;
    m_rawInputSkipCount = 0;
    refreshPointerSettings();
    m_hookOverrunCount = 0;
    m_hookMaxMicroseconds = 0;

    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = &InputCapture::windowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClassName;
    m_windowClassAtom = RegisterClassW(&windowClass);
    if (m_windowClassAtom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        releaseResources();
        return false;
    }

    m_window = CreateWindowExW(0, kWindowClassName, L"SyncControlInputCapture", 0,
                               0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    m_blankCursor = createTransparentCursor();

    WNDCLASSW cursorWindowClass{};
    cursorWindowClass.lpfnWndProc = &InputCapture::cursorWindowProc;
    cursorWindowClass.hInstance = instance;
    cursorWindowClass.lpszClassName = L"SyncControlCursorCoverWindow";
    m_cursorWindowClassAtom = RegisterClassW(&cursorWindowClass);
    if (m_cursorWindowClassAtom != 0) {
        m_cursorWindow = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
            MAKEINTRESOURCEW(m_cursorWindowClassAtom), L"", WS_POPUP,
            0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
        if (m_cursorWindow != nullptr) {
            SetLayeredWindowAttributes(m_cursorWindow, 0, kCursorCoverAlpha, LWA_ALPHA);
        }
    }

    if (!registerRawInput()) {
        Logger::write("input: failed to register raw mouse input");
        releaseResources();
        s_instance = nullptr;
        return false;
    }

    m_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, &InputCapture::mouseHookProc,
                                    instance, 0);
    m_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, &InputCapture::keyboardHookProc,
                                       instance, 0);
    if (m_mouseHook == nullptr || m_keyboardHook == nullptr) {
        releaseResources();
        s_instance = nullptr;
        return false;
    }
    return true;
}

void InputCapture::stop() {
    if (m_mouseHook == nullptr && m_keyboardHook == nullptr && m_window == nullptr) {
        return;
    }

    if (m_focus == Focus::Remote) {
        m_focus = Focus::Local;
        if (m_callbacks.sendSwitchOut && !m_focusPeerId.empty()) {
            m_callbacks.sendSwitchOut(m_focusPeerId);
        }
        m_focusPeerId.clear();
        showCursor();
    }
    releaseResources();
    s_instance = nullptr;
}

void InputCapture::releaseResources() {
    unregisterRawInput();
    if (m_mouseHook != nullptr) {
        UnhookWindowsHookEx(m_mouseHook);
        m_mouseHook = nullptr;
    }
    if (m_keyboardHook != nullptr) {
        UnhookWindowsHookEx(m_keyboardHook);
        m_keyboardHook = nullptr;
    }
    if (m_window != nullptr) {
        DestroyWindow(m_window);
        m_window = nullptr;
    }
    hideCursorCover();
    if (m_cursorWindow != nullptr) {
        DestroyWindow(m_cursorWindow);
        m_cursorWindow = nullptr;
    }
    if (m_blankCursor != nullptr) {
        DestroyCursor(m_blankCursor);
        m_blankCursor = nullptr;
    }
    if (m_cursorWindowClassAtom != 0) {
        UnregisterClassW(MAKEINTRESOURCEW(m_cursorWindowClassAtom),
                         GetModuleHandleW(nullptr));
        m_cursorWindowClassAtom = 0;
    }
    if (m_windowClassAtom != 0) {
        UnregisterClassW(MAKEINTRESOURCEW(m_windowClassAtom), GetModuleHandleW(nullptr));
        m_windowClassAtom = 0;
    }
    m_callbacks = {};
}

bool InputCapture::registerRawInput() {
    RAWINPUTDEVICE device{};
    device.usUsagePage = kGenericDesktopUsagePage;
    device.usUsage = kMouseUsage;
    device.dwFlags = RIDEV_INPUTSINK;
    device.hwndTarget = m_window;
    m_rawInputRegistered = RegisterRawInputDevices(&device, 1, sizeof(device)) != FALSE;
    return m_rawInputRegistered;
}

void InputCapture::unregisterRawInput() {
    if (!m_rawInputRegistered) {
        return;
    }
    RAWINPUTDEVICE device{};
    device.usUsagePage = kGenericDesktopUsagePage;
    device.usUsage = kMouseUsage;
    device.dwFlags = RIDEV_REMOVE;
    RegisterRawInputDevices(&device, 1, sizeof(device));
    m_rawInputRegistered = false;
}

LRESULT CALLBACK InputCapture::mouseHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (s_instance != nullptr && code >= 0) {
        LARGE_INTEGER start{};
        LARGE_INTEGER end{};
        QueryPerformanceCounter(&start);
        const LRESULT result = s_instance->handleMouseHook(
            wParam, reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam));
        QueryPerformanceCounter(&end);
        s_instance->recordHookTiming(start, end);
        return result;
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT CALLBACK InputCapture::keyboardHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (s_instance != nullptr && code >= 0) {
        LARGE_INTEGER start{};
        LARGE_INTEGER end{};
        QueryPerformanceCounter(&start);
        const LRESULT result = s_instance->handleKeyboardHook(
            wParam, reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam));
        QueryPerformanceCounter(&end);
        s_instance->recordHookTiming(start, end);
        return result;
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void InputCapture::recordHookTiming(LARGE_INTEGER start, LARGE_INTEGER end) {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    if (frequency.QuadPart == 0) {
        return;
    }
    const auto microseconds = static_cast<uint32_t>(
        (end.QuadPart - start.QuadPart) * 1000000 / frequency.QuadPart);
    m_hookMaxMicroseconds = std::max(m_hookMaxMicroseconds, microseconds);
    if (microseconds <= kHookTargetMicroseconds) {
        return;
    }
    ++m_hookOverrunCount;
    if (m_hookOverrunCount == 1 || m_hookOverrunCount % 1000 == 0) {
        Logger::write("input: hook callback exceeded 0.1ms (" +
                      std::to_string(microseconds) + "us)");
    }
}

LRESULT CALLBACK InputCapture::windowProc(HWND window, UINT message,
                                          WPARAM wParam, LPARAM lParam) {
    if (s_instance != nullptr && message == WM_INPUT) {
        s_instance->handleRawInput(reinterpret_cast<HRAWINPUT>(lParam));
    }
    if (s_instance != nullptr && message == WM_SETTINGCHANGE) {
        s_instance->refreshPointerSettings();
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT InputCapture::handleMouseHook(WPARAM wParam, const MSLLHOOKSTRUCT* info) {
    if (info == nullptr) {
        return CallNextHookEx(nullptr, 0, wParam, reinterpret_cast<LPARAM>(info));
    }

    if (m_focus == Focus::Remote) {
        if (wParam == WM_MOUSEMOVE) {
            ++m_hookRemoteMoveCount;
            const uint64_t nowMs = GetTickCount64();
            if (!m_rawInputDeliveryLogged &&
                (m_lastRawInputTickMs == 0 ||
                 nowMs - m_lastRawInputTickMs > 500)) {
                Logger::write("input: remote hook is receiving moves while raw input is silent");
                m_rawInputDeliveryLogged = true;
            }
            return 1;
        }

        switch (wParam) {
            case WM_LBUTTONDOWN:
                m_callbacks.sendMouseButton(MouseButtonCode::Left, MouseAction::Down);
                break;
            case WM_LBUTTONUP:
                m_callbacks.sendMouseButton(MouseButtonCode::Left, MouseAction::Up);
                break;
            case WM_RBUTTONDOWN:
                m_callbacks.sendMouseButton(MouseButtonCode::Right, MouseAction::Down);
                break;
            case WM_RBUTTONUP:
                m_callbacks.sendMouseButton(MouseButtonCode::Right, MouseAction::Up);
                break;
            case WM_MBUTTONDOWN:
                m_callbacks.sendMouseButton(MouseButtonCode::Middle, MouseAction::Down);
                break;
            case WM_MBUTTONUP:
                m_callbacks.sendMouseButton(MouseButtonCode::Middle, MouseAction::Up);
                break;
            case WM_XBUTTONDOWN: {
                const WORD xButton = HIWORD(info->mouseData);
                if (xButton == XBUTTON1) {
                    m_callbacks.sendMouseButton(MouseButtonCode::X1, MouseAction::Down);
                } else if (xButton == XBUTTON2) {
                    m_callbacks.sendMouseButton(MouseButtonCode::X2, MouseAction::Down);
                }
                break;
            }
            case WM_XBUTTONUP: {
                const WORD xButton = HIWORD(info->mouseData);
                if (xButton == XBUTTON1) {
                    m_callbacks.sendMouseButton(MouseButtonCode::X1, MouseAction::Up);
                } else if (xButton == XBUTTON2) {
                    m_callbacks.sendMouseButton(MouseButtonCode::X2, MouseAction::Up);
                }
                break;
            }
            case WM_MOUSEWHEEL:
                m_callbacks.sendMouseWheel(
                    WheelDirection::Vertical,
                    static_cast<int16_t>(HIWORD(info->mouseData)));
                break;
            case WM_MOUSEHWHEEL:
                m_callbacks.sendMouseWheel(
                    WheelDirection::Horizontal,
                    static_cast<int16_t>(HIWORD(info->mouseData)));
                break;
            default:
                break;
        }
        return 1;
    }

    if (wParam == WM_MOUSEMOVE) {
        const int screenW = GetSystemMetrics(SM_CXSCREEN);
        const int screenH = GetSystemMetrics(SM_CYSCREEN);
        if (screenW > 1 && screenH > 1) {
            const EdgeHit hit = detectEdge(info->pt.x, info->pt.y, screenW, screenH);
            if (hit.atEdge && trySwitchToRemote(hit.edge, info->pt, hit.ratio)) {
                return 1;
            }
        }
    }
    return CallNextHookEx(nullptr, 0, wParam, reinterpret_cast<LPARAM>(info));
}

LRESULT InputCapture::handleKeyboardHook(WPARAM wParam, const KBDLLHOOKSTRUCT* info) {
    if (info == nullptr) {
        return CallNextHookEx(nullptr, 0, wParam, reinterpret_cast<LPARAM>(info));
    }

    const bool isKeyEvent = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN ||
                            wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
    const bool isKeyDown = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
    const bool isKeyUp = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
    if (isKeyEvent) {
        if (isKeyDown) {
            if (info->vkCode == VK_LCONTROL || info->vkCode == VK_RCONTROL) {
                m_returnHotkeyCtrlDown = true;
            } else if (info->vkCode == VK_LMENU || info->vkCode == VK_RMENU) {
                m_returnHotkeyAltDown = true;
            }
        } else if (isKeyUp) {
            if (info->vkCode == VK_LCONTROL || info->vkCode == VK_RCONTROL) {
                m_returnHotkeyCtrlDown = false;
            } else if (info->vkCode == VK_LMENU || info->vkCode == VK_RMENU) {
                m_returnHotkeyAltDown = false;
            }
        }
    }

    if (isKeyEvent && isKeyDown && info->vkCode == VK_HOME) {
        const bool trackedCtrlDown = m_returnHotkeyCtrlDown;
        const bool trackedAltDown = m_returnHotkeyAltDown;
        const bool asyncCtrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool asyncAltDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        const bool hotkeyDown = (trackedCtrlDown || asyncCtrlDown) &&
                                (trackedAltDown || asyncAltDown);
        const bool modifierPresent = trackedCtrlDown || asyncCtrlDown ||
                                     trackedAltDown || asyncAltDown;
        if (modifierPresent) {
            Logger::write("input: return hotkey Home keydown focus=" +
                          std::string(m_focus == Focus::Remote ? "remote" : "local") +
                          " trackedCtrl=" + std::to_string(trackedCtrlDown) +
                          " trackedAlt=" + std::to_string(trackedAltDown) +
                          " asyncCtrl=" + std::to_string(asyncCtrlDown) +
                          " asyncAlt=" + std::to_string(asyncAltDown));
        }
        if (hotkeyDown) {
            if (m_focus == Focus::Remote) {
                Logger::write("input: return hotkey triggered peer=" + m_focusPeerId);
                releaseRemoteHotkeyModifiers();
                returnToLocalCenter();
            }
            return 1;
        }
    }

    if (m_focus == Focus::Remote) {
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            m_callbacks.sendKeyEvent(true, static_cast<uint16_t>(info->vkCode),
                                     static_cast<uint16_t>(info->scanCode),
                                     (info->flags & LLKHF_EXTENDED) != 0);
        } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
            m_callbacks.sendKeyEvent(false, static_cast<uint16_t>(info->vkCode),
                                     static_cast<uint16_t>(info->scanCode),
                                     (info->flags & LLKHF_EXTENDED) != 0);
        }
        return 1;
    }

    return CallNextHookEx(nullptr, 0, wParam, reinterpret_cast<LPARAM>(info));
}

bool InputCapture::trySwitchToRemote(Edge edge, const POINT& point, float ratio) {
    if (!m_callbacks.localScreenId || !m_callbacks.lookupRoute) {
        return false;
    }

    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    const std::string selfId = m_callbacks.localScreenId();
    const RouteTarget target = m_callbacks.lookupRoute(selfId, edge, ratio);

    if (!target.found) {
        // 空隙段：不切换，把光标拉回边缘内侧 1px 后吞掉事件
        switch (edge) {
            case Edge::Left:
                SetCursorPos(1, point.y);
                break;
            case Edge::Right:
                SetCursorPos(screenW - 2, point.y);
                break;
            case Edge::Top:
                SetCursorPos(point.x, 1);
                break;
            case Edge::Bottom:
                SetCursorPos(point.x, screenH - 2);
                break;
        }
        return true;
    }

    if (target.isMaster || target.peerId.empty() ||
        target.width == 0 || target.height == 0) {
        return false;
    }
    if (m_callbacks.clipboardSync) {
        m_callbacks.clipboardSync(std::string(), target.peerId);
    }
    if (!m_callbacks.sendSwitchIn ||
        !m_callbacks.sendSwitchIn(target.peerId, target.entryX, target.entryY)) {
        return false;
    }

    m_focusPeerId = target.peerId;
    m_remoteEntryEdge = target.toEdge;
    m_remoteWidth = target.width;
    m_remoteHeight = target.height;
    m_remoteX = target.entryX;
    m_remoteY = target.entryY;
    m_remoteEntered = false;
    m_rawInputSkipCount = 1;
    m_rawInputRemoteCount = 0;
    m_rawInputMoveCount = 0;
    m_hookRemoteMoveCount = 0;
    m_lastRawInputTickMs = 0;
    m_rawInputFailureLogged = false;
    m_rawInputDeliveryLogged = false;
    m_remoteInputModeLogged = false;
    m_remoteMoveLogged = false;
    m_lastAbsoluteDevice = nullptr;
    m_lastAbsoluteX = -1;
    m_lastAbsoluteY = -1;
    m_lastAbsoluteTickMs = 0;
    refreshPointerSettings();

    m_focus = Focus::Remote;
    hideCursor();
    Logger::write(std::string("input: switched to remote peer=") + target.peerId +
                  " edge=" + edgeName(edge) +
                  " entry=" + std::to_string(target.entryX) + "," +
                  std::to_string(target.entryY));
    return true;
}

void InputCapture::refreshPointerSettings() {
    UINT pointerSpeed = 10;
    if (!SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &pointerSpeed, 0)) {
        pointerSpeed = 10;
    }
    m_pointerSpeedMultiplier =
        static_cast<double>(std::clamp<UINT>(pointerSpeed, 1, 20)) / 10.0;

    int mouseSettings[3] = {6, 10, 0};
    if (SystemParametersInfoW(SPI_GETMOUSE, 0, mouseSettings, 0)) {
        m_pointerAccelerationThreshold1 =
            static_cast<double>(std::clamp(mouseSettings[0], 0, 1000));
        m_pointerAccelerationThreshold2 =
            static_cast<double>(std::clamp(mouseSettings[1], 0, 1000));
        m_pointerAccelerationLevel =
            static_cast<double>(std::clamp(mouseSettings[2], 0, 2));
    } else {
        m_pointerAccelerationThreshold1 = 6.0;
        m_pointerAccelerationThreshold2 = 10.0;
        m_pointerAccelerationLevel = 0.0;
    }
    m_lastPointerSettingsRefreshMs = GetTickCount64();
}

void InputCapture::moveRemote(int deltaX, int deltaY) {
    if (m_remoteWidth == 0 || m_remoteHeight == 0) {
        return;
    }

    double compensatedX = static_cast<double>(deltaX) * m_pointerSpeedMultiplier;
    double compensatedY = static_cast<double>(deltaY) * m_pointerSpeedMultiplier;
    if (m_pointerAccelerationLevel > 0.0) {
        const double distance = std::max(std::abs(deltaX), std::abs(deltaY));
        double acceleration = 1.0;
        if (distance > m_pointerAccelerationThreshold2) {
            acceleration = 2.0;
        } else if (distance > m_pointerAccelerationThreshold1) {
            acceleration = 1.5;
        }
        compensatedX *= acceleration;
        compensatedY *= acceleration;
    }

    const int localWidth = GetSystemMetrics(SM_CXSCREEN);
    const int localHeight = GetSystemMetrics(SM_CYSCREEN);
    const double scaleX = localWidth > 0
                              ? static_cast<double>(m_remoteWidth) / localWidth
                              : 1.0;
    const double scaleY = localHeight > 0
                              ? static_cast<double>(m_remoteHeight) / localHeight
                              : 1.0;

    m_remoteX = std::clamp(m_remoteX + compensatedX * scaleX, 0.0,
                           static_cast<double>(m_remoteWidth - 1));
    m_remoteY = std::clamp(m_remoteY + compensatedY * scaleY, 0.0,
                           static_cast<double>(m_remoteHeight - 1));
    const auto remoteX = clampToRange(
        static_cast<int>(std::lround(m_remoteX)), m_remoteWidth - 1);
    const auto remoteY = clampToRange(
        static_cast<int>(std::lround(m_remoteY)), m_remoteHeight - 1);
    if (!m_remoteEntered) {
        switch (m_remoteEntryEdge) {
            case Edge::Left:
                m_remoteEntered = m_remoteX >= kRemoteEntryInset;
                break;
            case Edge::Right:
                m_remoteEntered = m_remoteX <= m_remoteWidth - 1 - kRemoteEntryInset;
                break;
            case Edge::Top:
                m_remoteEntered = m_remoteY >= kRemoteEntryInset;
                break;
            case Edge::Bottom:
                m_remoteEntered = m_remoteY <= m_remoteHeight - 1 - kRemoteEntryInset;
                break;
        }
    }
    m_callbacks.sendMouseMove(static_cast<uint16_t>(remoteX),
                              static_cast<uint16_t>(remoteY));
    if (!m_remoteMoveLogged && (deltaX != 0 || deltaY != 0)) {
        Logger::write("input: queued first remote mouse move at " +
                      std::to_string(remoteX) + "," + std::to_string(remoteY));
        m_remoteMoveLogged = true;
    }
    if (m_remoteEntered) {
        handleRemoteEdge();
    }
}

void InputCapture::handleRemoteEdge() {
    if (m_remoteWidth == 0 || m_remoteHeight == 0 || m_focusPeerId.empty()) {
        return;
    }

    const int remoteX = clampToRange(
        static_cast<int>(std::lround(m_remoteX)), m_remoteWidth - 1);
    const int remoteY = clampToRange(
        static_cast<int>(std::lround(m_remoteY)), m_remoteHeight - 1);
    const EdgeHit hit = detectEdge(remoteX, remoteY, m_remoteWidth, m_remoteHeight);
    if (!hit.atEdge || !m_callbacks.lookupRoute) {
        return;
    }

    const RouteTarget target = m_callbacks.lookupRoute(m_focusPeerId, hit.edge, hit.ratio);
    if (!target.found) {
        // 空隙段：远程光标已被钳制在边缘内侧，保持不动
        return;
    }
    if (target.isMaster) {
        returnToLocal(target);
        return;
    }
    if (target.peerId.empty() || target.peerId == m_focusPeerId ||
        target.width == 0 || target.height == 0 || !m_callbacks.sendSwitchIn) {
        return;
    }
    if (m_callbacks.clipboardSync) {
        m_callbacks.clipboardSync(m_focusPeerId, target.peerId);
    }
    if (m_callbacks.sendSwitchIn(target.peerId, target.entryX, target.entryY)) {
        if (m_callbacks.sendSwitchOut) {
            m_callbacks.sendSwitchOut(m_focusPeerId);
        }
        m_focusPeerId = target.peerId;
        m_remoteWidth = target.width;
        m_remoteHeight = target.height;
        m_remoteX = target.entryX;
        m_remoteY = target.entryY;
        m_remoteEntryEdge = target.toEdge;
        m_remoteEntered = false;
        m_rawInputSkipCount = 1;
        refreshPointerSettings();
    }
}

void InputCapture::returnToLocal(const RouteTarget& target) {
    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    int x = target.entryX;
    int y = target.entryY;
    switch (target.toEdge) {
        case Edge::Left:
            x = kReturnCursorInset;
            break;
        case Edge::Right:
            x = screenW - 1 - kReturnCursorInset;
            break;
        case Edge::Top:
            y = kReturnCursorInset;
            break;
        case Edge::Bottom:
            y = screenH - 1 - kReturnCursorInset;
            break;
    }
    returnToLocalAt(x, y);
}

void InputCapture::returnToLocalCenter() {
    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    returnToLocalAt(screenW / 2, screenH / 2);
}

void InputCapture::returnToLocalAt(int x, int y) {
    if (m_callbacks.clipboardSync && !m_focusPeerId.empty()) {
        m_callbacks.clipboardSync(m_focusPeerId, std::string());
    }
    if (m_callbacks.sendSwitchOut && !m_focusPeerId.empty()) {
        m_callbacks.sendSwitchOut(m_focusPeerId);
    }
    m_focus = Focus::Local;
    m_focusPeerId.clear();
    m_remoteEntryEdge = Edge::Top;
    m_remoteWidth = 0;
    m_remoteHeight = 0;
    m_remoteX = 0;
    m_remoteY = 0;
    m_remoteEntered = false;

    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    SetCursorPos(clampToRange(x, screenW - 1), clampToRange(y, screenH - 1));
    showCursor();
}

void InputCapture::releaseRemoteHotkeyModifiers() {
    const struct {
        UINT vkCode;
        bool extendedKey;
    } keys[] = {
        {VK_LCONTROL, false},
        {VK_RCONTROL, true},
        {VK_LMENU, false},
        {VK_RMENU, true},
    };

    for (const auto& key : keys) {
        const UINT scanCode = MapVirtualKeyW(key.vkCode, MAPVK_VK_TO_VSC);
        m_callbacks.sendKeyEvent(false, static_cast<uint16_t>(key.vkCode),
                                 static_cast<uint16_t>(scanCode), key.extendedKey);
    }
}

void InputCapture::hideCursor() {
    if (m_systemCursorHidden) {
        return;
    }

    const int width = GetSystemMetrics(SM_CXCURSOR);
    const int height = GetSystemMetrics(SM_CYCURSOR);
    if (width <= 0 || height <= 0) {
        return;
    }

    const UINT shapes[] = {
        OCR_NORMAL, OCR_IBEAM, OCR_WAIT, OCR_CROSS, OCR_UP,
        OCR_SIZENWSE, OCR_SIZENESW, OCR_SIZEWE, OCR_SIZENS,
        OCR_SIZEALL, OCR_NO, OCR_HAND, OCR_APPSTARTING,
    };
    bool hidden = false;
    bool normalHidden = false;
    for (const UINT shape : shapes) {
        const HCURSOR cursor = createTransparentCursor();
        if (cursor != nullptr && SetSystemCursor(cursor, shape) != FALSE) {
            hidden = true;
            if (shape == OCR_NORMAL) {
                normalHidden = true;
            }
        } else if (cursor != nullptr) {
            DestroyCursor(cursor);
        }
    }
    m_systemCursorHidden = hidden;
    if (!hidden || !normalHidden) {
        Logger::write("input: failed to hide the system cursor");
    }
    POINT point{};
    if (!GetCursorPos(&point)) {
        point = POINT{0, 0};
    }
    showCursorCover(point);
}

void InputCapture::showCursor() {
    if (!m_systemCursorHidden) {
        return;
    }
    hideCursorCover();
    if (!SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE)) {
        Logger::write("input: failed to restore the system cursor");
    }
    m_systemCursorHidden = false;
}

void InputCapture::showCursorCover(const POINT& point) {
    if (m_cursorWindow == nullptr || m_blankCursor == nullptr) {
        return;
    }
    SetLayeredWindowAttributes(m_cursorWindow, 0, kCursorCoverAlpha, LWA_ALPHA);
    SetWindowPos(m_cursorWindow, HWND_TOPMOST, point.x, point.y, 1, 1,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void InputCapture::hideCursorCover() {
    if (m_cursorWindow != nullptr && IsWindowVisible(m_cursorWindow)) {
        ShowWindow(m_cursorWindow, SW_HIDE);
    }
}

LRESULT CALLBACK InputCapture::cursorWindowProc(HWND window, UINT message,
                                                WPARAM wParam, LPARAM lParam) {
    if (message == WM_SETCURSOR && s_instance != nullptr &&
        s_instance->m_blankCursor != nullptr) {
        SetCursor(s_instance->m_blankCursor);
        return TRUE;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void InputCapture::handleRawInput(HRAWINPUT input) {
    alignas(RAWINPUT) BYTE buffer[sizeof(RAWINPUT) + 64] = {};
    UINT size = sizeof(buffer);
    const UINT copied = GetRawInputData(input, RID_INPUT, buffer, &size,
                                        sizeof(RAWINPUTHEADER));
    ++m_rawInputReceivedCount;
    m_lastRawInputTickMs = GetTickCount64();
    if (copied == static_cast<UINT>(-1) ||
        copied < sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE)) {
        if (!m_rawInputFailureLogged) {
            Logger::write("input: GetRawInputData failed, copied=" +
                          std::to_string(copied));
            m_rawInputFailureLogged = true;
        }
        return;
    }

    const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(buffer);
    if (raw->header.dwType != RIM_TYPEMOUSE) {
        return;
    }
    const RAWMOUSE& mouse = raw->data.mouse;
    if (m_focus != Focus::Remote) {
        return;
    }
    ++m_rawInputRemoteCount;
    const uint64_t nowMs = GetTickCount64();
    if (nowMs - m_lastPointerSettingsRefreshMs >= 1000) {
        refreshPointerSettings();
    }
    if (!m_remoteInputModeLogged) {
        const bool absolute = (mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0;
        Logger::write(std::string("input: raw mouse mode=") +
                      (absolute ? "absolute" : "relative") +
                      " flags=" + std::to_string(mouse.usFlags) +
                      " delta=" + std::to_string(mouse.lLastX) + "," +
                      std::to_string(mouse.lLastY));
        m_remoteInputModeLogged = true;
    }

    if ((mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0) {
        const bool valid = raw->header.hDevice == m_lastAbsoluteDevice &&
                           m_lastAbsoluteX >= 0 && m_lastAbsoluteY >= 0 &&
                           nowMs - m_lastAbsoluteTickMs <= kAbsoluteResetMs;
        int deltaX = 0;
        int deltaY = 0;
        if (valid) {
            const bool virtualDesktop =
                (mouse.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
            const int extentW = GetSystemMetrics(virtualDesktop
                                                     ? SM_CXVIRTUALSCREEN
                                                     : SM_CXSCREEN);
            const int extentH = GetSystemMetrics(virtualDesktop
                                                     ? SM_CYVIRTUALSCREEN
                                                     : SM_CYSCREEN);
            if (extentW > 0 && extentH > 0) {
                const double scaledX =
                    (static_cast<double>(mouse.lLastX) - m_lastAbsoluteX) *
                    extentW / 65535.0;
                const double scaledY =
                    (static_cast<double>(mouse.lLastY) - m_lastAbsoluteY) *
                    extentH / 65535.0;
                deltaX = scaledX >= 0 ? static_cast<int>(scaledX + 0.5)
                                      : -static_cast<int>(-scaledX + 0.5);
                deltaY = scaledY >= 0 ? static_cast<int>(scaledY + 0.5)
                                      : -static_cast<int>(-scaledY + 0.5);
            }
        }
        m_lastAbsoluteDevice = raw->header.hDevice;
        m_lastAbsoluteX = mouse.lLastX;
        m_lastAbsoluteY = mouse.lLastY;
        m_lastAbsoluteTickMs = nowMs;
        if (deltaX == 0 && deltaY == 0) {
            return;
        }
        moveRemote(deltaX, deltaY);
        return;
    }

    if (mouse.lLastX == 0 && mouse.lLastY == 0) {
        return;
    }
    if (m_rawInputSkipCount > 0) {
        --m_rawInputSkipCount;
        return;
    }
    ++m_rawInputMoveCount;
    moveRemote(mouse.lLastX, mouse.lLastY);
}

}  // namespace synccontrol
