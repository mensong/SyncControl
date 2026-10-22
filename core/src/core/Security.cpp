
#include "core/Security.h"

#include <Windows.h>
#include <bcrypt.h>
#include <AclAPI.h>

#include <cctype>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace synccontrol {
namespace {

constexpr size_t kKeyBytes = 32;
constexpr size_t kHexKeyLength = kKeyBytes * 2;

std::wstring keyDirectory() {
    char buffer[MAX_PATH]{};
    UINT size = GetEnvironmentVariableA("LOCALAPPDATA", buffer, MAX_PATH);
    if (size == 0 || size > MAX_PATH) {
        size = GetTempPathA(MAX_PATH, buffer);
        if (size == 0 || size > MAX_PATH) {
            return {};
        }
    }
    std::wstring directory(buffer, buffer + size);
    while (!directory.empty() && (directory.back() == L'\\' || directory.back() == L'/')) {
        directory.pop_back();
    }
    directory += L"\\SyncControl";
    CreateDirectoryW(directory.c_str(), nullptr);
    return directory;
}

std::wstring keyFilePath() {
    const std::wstring directory = keyDirectory();
    return directory.empty() ? std::wstring() : directory + L"\\key.bin";
}

bool isValidKey(const std::string& value) {
    if (value.empty() || value.size() > 1024) {
        return false;
    }
    for (char c : value) {
        if (c == '\r' || c == '\n' || c == '\0') {
            return false;
        }
    }
    return true;
}

std::string randomHexKey() {
    unsigned char bytes[kKeyBytes]{};
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        return {};
    }

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned char byte : bytes) {
        output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return output.str();
}

std::string readKey() {
    const std::wstring path = keyFilePath();
    if (path.empty()) {
        return {};
    }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }

    char buffer[2048]{};
    DWORD read = 0;
    const bool ok = ReadFile(file, buffer, sizeof(buffer) - 1, &read, nullptr);
    CloseHandle(file);
    if (!ok || read == 0) {
        return {};
    }

    std::string value(buffer, read);
    while (!value.empty() &&
           (value.back() == '\n' || value.back() == '\r' || value.back() == ' ')) {
        value.pop_back();
    }
    return isValidKey(value) ? value : std::string();
}

bool restrictToCurrentUser(const std::wstring& path) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    DWORD userBytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &userBytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || userBytes == 0) {
        CloseHandle(token);
        return false;
    }

    std::vector<BYTE> userBuffer(userBytes);
    if (!GetTokenInformation(token, TokenUser, userBuffer.data(), userBytes, &userBytes)) {
        CloseHandle(token);
        return false;
    }
    CloseHandle(token);

    const SID* sid = reinterpret_cast<const SID*>(
        reinterpret_cast<const TOKEN_USER*>(userBuffer.data())->User.Sid);
    const ULONG aclSize = sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) +
                          SECURITY_MAX_SID_SIZE;
    std::vector<BYTE> aclBuffer(aclSize, 0);
    ACL* acl = reinterpret_cast<ACL*>(aclBuffer.data());
    if (!InitializeAcl(acl, aclSize, ACL_REVISION) ||
        !AddAccessAllowedAce(acl, ACL_REVISION, FILE_ALL_ACCESS,
                             const_cast<SID*>(sid))) {
        return false;
    }

    return SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                                 DACL_SECURITY_INFORMATION |
                                     PROTECTED_DACL_SECURITY_INFORMATION,
                                 nullptr, nullptr, acl, nullptr) == ERROR_SUCCESS;
}

}  // namespace

std::string sha256Hex(const std::string& value) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string output;
    unsigned char digest[32]{};
    DWORD digestSize = sizeof(digest);
    DWORD objectSize = 0;
    DWORD copied = 0;

    do {
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
            BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                              reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
                              &copied, 0) < 0) {
            break;
        }

        std::vector<BYTE> object(std::max<DWORD>(objectSize, 1));
        if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) < 0 ||
            BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())),
                           static_cast<ULONG>(value.size()), 0) < 0 ||
            BCryptFinishHash(hash, digest, digestSize, 0) < 0) {
            break;
        }

        std::ostringstream text;
        text << std::hex << std::setfill('0');
        for (unsigned char byte : digest) {
            text << std::setw(2) << static_cast<unsigned int>(byte);
        }
        output = text.str();
    } while (false);

    if (hash != nullptr) {
        BCryptDestroyHash(hash);
    }
    if (algorithm != nullptr) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    return output;
}

bool SecurityKeyStore::saveKey(const std::string& hexKey) {
    if (!isValidKey(hexKey)) {
        return false;
    }
    const std::wstring path = keyFilePath();
    if (path.empty()) {
        return false;
    }

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool writtenOk = WriteFile(file, hexKey.data(),
                                     static_cast<DWORD>(hexKey.size()), &written,
                                     nullptr);
    CloseHandle(file);
    return writtenOk && restrictToCurrentUser(path);
}

std::string SecurityKeyStore::load() {
    return readKey();
}

std::string SecurityKeyStore::loadOrCreate() {
    std::string key = readKey();
    if (!key.empty()) {
        return key;
    }
    key = randomHexKey();
    if (key.empty() || !saveKey(key)) {
        return {};
    }
    return key;
}

bool SecurityKeyStore::importKey(const std::string& hexKey) {
    return saveKey(hexKey);
}

bool SecurityKeyStore::clearKey() {
    const std::wstring path = keyFilePath();
    return path.empty() || DeleteFileW(path.c_str()) != FALSE ||
           GetLastError() == ERROR_FILE_NOT_FOUND;
}

std::string SecurityKeyStore::regenerate() {
    const std::string key = randomHexKey();
    return !key.empty() && saveKey(key) ? key : std::string();
}

}  // namespace synccontrol
