// nsp_autostart.cpp — see nsp_autostart.h.

#include "nsp_autostart.h"

#include "nsp_common.h"

#include <string>
#include <vector>

namespace nsp {
namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kApprovedKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr wchar_t kValue[] = L"Nova SilkPlay";

std::string StatusText(const char* step, LSTATUS st) {
    char buf[256];
    _snprintf_s(buf, _TRUNCATE, "autostart: %s failed (error %ld)", step, static_cast<long>(st));
    return buf;
}

// The quoted full path of this executable, and nothing else. An autostart line
// carrying flags reads as suspicious to a user inspecting it, and there is
// nothing we need to pass.
std::wstring SelfCommand() {
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return std::wstring();
        if (n < buf.size() - 1) return L"\"" + std::wstring(buf.data(), n) + L"\"";
        buf.resize(buf.size() * 2);
        if (buf.size() > 65536) return std::wstring();
    }
}

// The Run value's string, or empty when there is none.
std::wstring ReadRunValue() {
    wchar_t data[1024] = {};
    DWORD cb = sizeof(data);
    const LSTATUS st =
        RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValue, RRF_RT_REG_SZ, nullptr, data, &cb);
    if (st != ERROR_SUCCESS) return std::wstring();
    return std::wstring(data);
}

}  // namespace

AutostartState AutostartQuery() {
    if (ReadRunValue().empty()) return AutostartState::kOff;

    // Task Manager > Startup apps records its own enable bit here. The encoding
    // is not documented but is stable and well observed: 0x02 enabled, 0x03
    // disabled, and 0x06/0x07 the same pair after a reboot — the low bit is the
    // "disabled" flag. Anything we do not recognise, and a missing value (the
    // state before Task Manager has ever touched the entry), counts as enabled,
    // which is what the shell will actually do.
    BYTE blob[64] = {};
    DWORD cb = sizeof(blob);
    const LSTATUS st =
        RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, kValue, RRF_RT_REG_BINARY, nullptr, blob, &cb);
    if (st == ERROR_SUCCESS && cb >= 1 && (blob[0] & 1) != 0) return AutostartState::kBlocked;
    return AutostartState::kOn;
}

bool AutostartSet(bool on, std::string* err) {
    if (on) {
        const std::wstring cmd = SelfCommand();
        if (cmd.empty()) {
            if (err) *err = "autostart: this executable's own path could not be read";
            return false;
        }
        HKEY key = nullptr;
        LSTATUS st = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE,
                                     nullptr, &key, nullptr);
        if (st != ERROR_SUCCESS) {
            if (err) *err = StatusText("opening the Run key", st);
            return false;
        }
        st = RegSetValueExW(key, kValue, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(cmd.c_str()),
                            static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
        if (st != ERROR_SUCCESS) {
            if (err) *err = StatusText("writing the Run value", st);
            return false;
        }
        Log("autostart: ON - %s", Narrow(cmd).c_str());
        return true;
    }

    const LSTATUS st = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kValue);
    if (st != ERROR_SUCCESS && st != ERROR_FILE_NOT_FOUND) {
        if (err) *err = StatusText("deleting the Run value", st);
        return false;
    }
    Log("autostart: OFF");
    return true;
}

bool AutostartRepair() {
    const std::wstring have = ReadRunValue();
    if (have.empty()) return true;  // not registered: nothing to repair, and we never add one
    const std::wstring want = SelfCommand();
    if (want.empty() || _wcsicmp(have.c_str(), want.c_str()) == 0) return true;

    // ONLY WHEN THE OLD TARGET IS GONE. "The path differs" is true of every copy
    // of this program - a build in the repository, an unpacked zip, a copy run
    // once out of curiosity - and repairing on that alone silently pointed the
    // user's autostart at whichever copy ran last, then the installed one pointed
    // it back, and the two fought over the key (review, 2026-09-20). A
    // reinstall into another folder leaves the old path NOT EXISTING, which is
    // the case this exists for and the only one it acts on.
    std::wstring old = have;
    if (!old.empty() && old.front() == L'"') {
        const size_t end = old.find(L'"', 1);
        old = end == std::wstring::npos ? old.substr(1) : old.substr(1, end - 1);
    }
    if (GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    std::string err;
    if (!AutostartSet(true, &err)) {
        LogErr("%s", err.c_str());
        return false;
    }
    Log("autostart: the Run value named %s and was repaired to this executable",
        Narrow(have).c_str());
    return true;
}

}  // namespace nsp
