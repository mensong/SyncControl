#pragma once

#include <string>

namespace synccontrol {

// Append-only service log: OutputDebugStringA plus a file under
// %LOCALAPPDATA%\SyncControl, rotated at 10 MB. Not for hot input paths.
class Logger {
public:
    static void write(const std::string& message);
};

}  // namespace synccontrol
