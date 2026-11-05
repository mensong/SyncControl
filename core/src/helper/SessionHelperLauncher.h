#pragma once

#include <Windows.h>

#include <string>

namespace synccontrol {

// Launched by the --service process (session 0, LocalSystem). Starts a copy of
// the core executable as --helper with the SYSTEM token re-anchored into the
// active console session, restarts it if it exits, and terminates it on stop.
class SessionHelperLauncher {
public:
    // Blocks until stopEvent fires. The live helper process handle (if any) is
    // written to helperProcessOut so the caller can terminate it on shutdown.
    // Waits for an active console session instead of failing when none exists
    // (boot time, logon transitions); always returns true on stop.
    static bool runSupervisorLoop(HANDLE stopEvent, HANDLE* helperProcessOut);
};

}  // namespace synccontrol
