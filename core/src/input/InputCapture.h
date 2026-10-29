#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef OEMRESOURCE
#define OEMRESOURCE
#endif
#include <Windows.h>

#include <cstdint>
#include <functional>
#include <string>

#include "layout/EdgeRouter.h"
#include "net/Protocol.h"

namespace synccontrol {

class InputCapture {
public:
    // Result of an edge route lookup performed by the host application.
    // isMaster means the target is the master screen (switch back to local).
    struct RouteTarget {
        bool found = false;
        bool isMaster = false;
        std::string peerId;
        Edge toEdge = Edge::Top;
        uint16_t entryX = 0;
        uint16_t entryY = 0;
        uint16_t width = 0;
        uint16_t height = 0;
    };

    struct HookProfile {
        uint64_t overruns = 0;
        uint32_t maxMicroseconds = 0;
    };

    struct Callbacks {
        std::function<void(uint16_t, uint16_t)> sendMouseMove;
        std::function<void(MouseButtonCode, MouseAction)> sendMouseButton;
        std::function<void(WheelDirection, int16_t)> sendMouseWheel;
        std::function<void(bool, uint16_t, uint16_t, bool)> sendKeyEvent;
        std::function<std::string()> localScreenId;
        std::function<RouteTarget(const std::string& screenId, Edge edge, float ratio)> lookupRoute;
        std::function<bool(const std::string& peerId, uint16_t x, uint16_t y)> sendSwitchIn;
        std::function<bool(const std::string& peerId)> sendSwitchOut;
        // fromPeerId 为空表示源是 Master 本机；toPeerId 为空表示目标是 Master 本机。
        std::function<void(const std::string& fromPeerId, const std::string& toPeerId)> clipboardSync;
    };

    bool start(const Callbacks& callbacks);
    void stop();
    HookProfile hookProfile() const { return {m_hookOverrunCount, m_hookMaxMicroseconds}; }

private:
    enum class Focus {
        Local,
        Remote,
    };

    static LRESULT CALLBACK mouseHookProc(int code, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK keyboardHookProc(int code, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK windowProc(HWND window, UINT message,
                                       WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK cursorWindowProc(HWND window, UINT message,
                                             WPARAM wParam, LPARAM lParam);

    LRESULT handleMouseHook(WPARAM wParam, const MSLLHOOKSTRUCT* info);
    LRESULT handleKeyboardHook(WPARAM wParam, const KBDLLHOOKSTRUCT* info);
    void handleRawInput(HRAWINPUT input);

    bool trySwitchToRemote(Edge edge, const POINT& point, float ratio);
    void refreshPointerSettings();
    void moveRemote(int deltaX, int deltaY);
    void handleRemoteEdge();
    void returnToLocal(const RouteTarget& target);
    void returnToLocalCenter();
    void returnToLocalAt(int x, int y);
    void releaseRemoteHotkeyModifiers();
    void hideCursor();
    void showCursor();
    void showCursorCover(const POINT& point);
    void hideCursorCover();
    void releaseResources();
    bool registerRawInput();
    void unregisterRawInput();
    void recordHookTiming(LARGE_INTEGER start, LARGE_INTEGER end);

    static InputCapture* s_instance;

    Callbacks m_callbacks;
    HWND m_window = nullptr;
    HHOOK m_mouseHook = nullptr;
    HHOOK m_keyboardHook = nullptr;
    ATOM m_windowClassAtom = 0;
    Focus m_focus = Focus::Local;
    std::string m_focusPeerId;
    bool m_returnHotkeyCtrlDown = false;
    bool m_returnHotkeyAltDown = false;
    Edge m_remoteEntryEdge = Edge::Top;
    double m_remoteX = 0;
    double m_remoteY = 0;
    double m_pointerSpeedMultiplier = 1.0;
    double m_pointerAccelerationThreshold1 = 6.0;
    double m_pointerAccelerationThreshold2 = 10.0;
    double m_pointerAccelerationLevel = 0.0;
    uint16_t m_remoteWidth = 0;
    uint16_t m_remoteHeight = 0;
    bool m_systemCursorHidden = false;
    bool m_rawInputRegistered = false;
    UINT m_rawInputSkipCount = 0;
    bool m_remoteEntered = false;
    HCURSOR m_blankCursor = nullptr;
    HWND m_cursorWindow = nullptr;
    ATOM m_cursorWindowClassAtom = 0;
    uint64_t m_hookOverrunCount = 0;
    uint32_t m_hookMaxMicroseconds = 0;
    uint64_t m_rawInputReceivedCount = 0;
    uint64_t m_rawInputRemoteCount = 0;
    uint64_t m_rawInputMoveCount = 0;
    uint64_t m_hookRemoteMoveCount = 0;
    uint64_t m_lastRawInputTickMs = 0;
    uint64_t m_lastPointerSettingsRefreshMs = 0;
    bool m_rawInputFailureLogged = false;
    bool m_rawInputDeliveryLogged = false;
    bool m_remoteInputModeLogged = false;
    bool m_remoteMoveLogged = false;
    HANDLE m_lastAbsoluteDevice = nullptr;
    int m_lastAbsoluteX = -1;
    int m_lastAbsoluteY = -1;
    uint64_t m_lastAbsoluteTickMs = 0;
};

}  // namespace synccontrol
