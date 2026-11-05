#pragma once

#include <string>

namespace synccontrol {

// Append-only service log: OutputDebugStringA plus a file under
// %LOCALAPPDATA%\SyncControl, rotated at 10 MB. Not for hot input paths.
// The --helper process logs to C:\ProgramData\SyncControl instead: SYSTEM's
// LOCALAPPDATA points at the system profile where users never look.
class Logger {
public:
    static void write(const std::string& message);
    // Must be called before the first write() in the --helper process.
    static void setHelperMode();
    static bool isHelperMode();
};

}  // namespace synccontrol
