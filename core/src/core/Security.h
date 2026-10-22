#pragma once

#include <string>

namespace synccontrol {

// Stores a non-empty key in the current user's profile. An empty key means
// explicit no-key mode; master and slave must make the same choice.
class SecurityKeyStore {
public:
    static std::string load();
    static std::string loadOrCreate();
    static bool importKey(const std::string& hexKey);
    static bool clearKey();
    static std::string regenerate();

private:
    static bool saveKey(const std::string& hexKey);
};

std::string sha256Hex(const std::string& value);

}  // namespace synccontrol
