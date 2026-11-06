#pragma once

#include <string>

namespace synccontrol {

// Manages the SyncControlHelper Windows service. The service runs the core
// executable with --service, which launches a session helper (SYSTEM token in
// the interactive session) that can follow the input desktop across the
// secure desktop (UAC / Winlogon) boundary.
//
// Install requires a caller holding SeCreatePagefilePrivilege... i.e. an
// administrator; the core already runs elevated (requireAdministrator).
class HelperServiceControl {
public:
    static bool install(std::string& error);
    static bool uninstall(std::string& error);
    // Stops the service without deleting it; used when core exits so the
    // idle SYSTEM helper does not linger.
    static bool uninstallForStop();
    static bool isInstalled();
    static bool isRunning();
};

}  // namespace synccontrol
