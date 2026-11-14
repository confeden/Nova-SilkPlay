// nsp_autostart.cpp — current-user Startup shortcut, with legacy Run migration.
#include "nsp_autostart.h"
#include "nsp_common.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <iterator>
#include <vector>

namespace nsp {
namespace {
using Microsoft::WRL::ComPtr;
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kApprovedRun[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr wchar_t kApprovedFolder[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder";
constexpr wchar_t kValue[] = L"Nova SilkPlay";
constexpr wchar_t kShortcut[] = L"Nova SilkPlay.lnk";

bool Fail(const char* step, HRESULT hr, std::string* err) {
    if (err) *err = std::string("autostart: ") + step + " failed: " + HrString(hr);
    return false;
}
std::wstring SelfPath() {
    std::vector<wchar_t> buf(MAX_PATH);
    while (buf.size() <= 65536) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size() - 1) return std::wstring(buf.data(), n);
        buf.resize(buf.size() * 2);
    }
    return {};
}
bool ShortcutPath(std::wstring* path, std::string* err) {
    PWSTR dir = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_Startup, KF_FLAG_CREATE, nullptr, &dir);
    if (FAILED(hr)) return Fail("locating the user's Startup folder", hr, err);
    *path = std::wstring(dir) + L"\\" + kShortcut;
    CoTaskMemFree(dir);
    return true;
}
std::wstring ReadRunValue() {
    wchar_t data[32768]{};
    DWORD cb = sizeof(data);
    const LSTATUS st = RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValue, RRF_RT_REG_SZ, nullptr, data, &cb);
    if (st != ERROR_SUCCESS && st != ERROR_FILE_NOT_FOUND && st != ERROR_PATH_NOT_FOUND)
        LogErr("autostart: reading the legacy Run entry failed (%ld)", st);
    return st == ERROR_SUCCESS ? std::wstring(data) : std::wstring();
}
bool Blocked(const wchar_t* key, const wchar_t* name) {
    BYTE blob[64]{};
    DWORD cb = sizeof(blob);
    const LSTATUS st = RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_BINARY, nullptr, blob, &cb);
    if (st != ERROR_SUCCESS && st != ERROR_FILE_NOT_FOUND && st != ERROR_PATH_NOT_FOUND) {
        LogErr("autostart: reading StartupApproved failed (%ld) - preserving a possible disable", st);
        return true;
    }
    return st == ERROR_SUCCESS && cb >= 1 && (blob[0] & 1) != 0;
}
bool TargetMissing(const std::wstring& path, bool* missing, std::string* err) {
    *missing = false;
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
        *missing = true;
        return true;
    }
    return Fail("checking the existing shortcut target", HRESULT_FROM_WIN32(code), err);
}
bool RemoveLegacy(std::string* err) {
    const LSTATUS st = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kValue);
    if (st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND || st == ERROR_PATH_NOT_FOUND) return true;
    return Fail("removing the legacy Run entry", HRESULT_FROM_WIN32(st), err);
}
bool LoadShortcut(const std::wstring& path, std::wstring* target, std::string* err) {
    ComPtr<IShellLinkW> link;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    ComPtr<IPersistFile> file;
    if (SUCCEEDED(hr)) hr = link.As(&file);
    if (SUCCEEDED(hr)) hr = file->Load(path.c_str(), STGM_READ);
    wchar_t buf[32768]{};
    if (SUCCEEDED(hr)) hr = link->GetPath(buf, static_cast<int>(std::size(buf)), nullptr, SLGP_RAWPATH);
    if (FAILED(hr)) return Fail("reading the Startup shortcut", hr, err);
    *target = buf;
    return !target->empty() || Fail("reading the shortcut target", E_FAIL, err);
}
bool SaveShortcut(const std::wstring& path, const std::wstring& target, std::string* err) {
    if (target.empty()) return Fail("reading this executable's path", E_FAIL, err);
    ComPtr<IShellLinkW> link;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (SUCCEEDED(hr)) hr = link->SetPath(target.c_str());
    if (SUCCEEDED(hr)) hr = link->SetArguments(L"");
    const size_t slash = target.find_last_of(L"\\/");
    if (SUCCEEDED(hr) && slash != std::wstring::npos)
        hr = link->SetWorkingDirectory(target.substr(0, slash).c_str());
    if (SUCCEEDED(hr)) hr = link->SetIconLocation(target.c_str(), 0);
    if (SUCCEEDED(hr)) hr = link->SetDescription(L"Nova SilkPlay");
    ComPtr<IPersistFile> file;
    if (SUCCEEDED(hr)) hr = link.As(&file);
    if (FAILED(hr)) return Fail("creating the Startup shortcut", hr, err);
    // Stage outside Startup: Windows may open any file left there at sign-in.
    // The parent normally shares its volume, preserving atomic replacement.
    const std::wstring dir = path.substr(0, path.find_last_of(L"\\/"));
    const std::wstring stagingDir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    wchar_t tmp[MAX_PATH]{};
    if (!GetTempFileNameW(stagingDir.c_str(), L"nsp", 0, tmp))
        return Fail("creating a temporary shortcut", HRESULT_FROM_WIN32(GetLastError()), err);
    hr = file->Save(tmp, TRUE);
    if (SUCCEEDED(hr) && !MoveFileExW(tmp, path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        hr = HRESULT_FROM_WIN32(GetLastError());
    if (FAILED(hr)) {
        if (!DeleteFileW(tmp) && GetLastError() != ERROR_FILE_NOT_FOUND)
            LogErr("autostart: temporary shortcut cleanup failed (%lu)", GetLastError());
        return Fail("saving the Startup shortcut", hr, err);
    }
    return true;
}
}  // namespace

AutostartState AutostartQuery() {
    std::wstring path;
    std::string err;
    if (!ShortcutPath(&path, &err)) { LogErr("%s", err.c_str()); return AutostartState::kOff; }
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring target;
        if (!LoadShortcut(path, &target, &err)) {
            LogErr("%s", err.c_str());
            return AutostartState::kOff;
        }
        return Blocked(kApprovedFolder, kShortcut) ? AutostartState::kBlocked : AutostartState::kOn;
    }
    if (ReadRunValue().empty()) return AutostartState::kOff;
    return Blocked(kApprovedRun, kValue) ? AutostartState::kBlocked : AutostartState::kOn;
}
bool AutostartSet(bool on, std::string* err) {
    std::wstring path;
    if (!ShortcutPath(&path, err)) return false;
    if (on) {
        if (!SaveShortcut(path, SelfPath(), err)) return false;
    } else if (!DeleteFileW(path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND &&
               GetLastError() != ERROR_PATH_NOT_FOUND) {
        return Fail("deleting the Startup shortcut", HRESULT_FROM_WIN32(GetLastError()), err);
    }
    if (!RemoveLegacy(err)) return false;
    Log("autostart: %s - %s", on ? "ON" : "OFF", Narrow(path).c_str());
    return true;
}
bool AutostartRepair() {
    std::wstring path;
    std::string err;
    if (!ShortcutPath(&path, &err)) { LogErr("%s", err.c_str()); return false; }
    const std::wstring legacy = ReadRunValue();
    std::wstring target;
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (!LoadShortcut(path, &target, &err)) { LogErr("%s", err.c_str()); return false; }
        // A development build must never steal an installed copy's working shortcut.
        bool missing = false;
        if (!TargetMissing(target, &missing, &err)) { LogErr("%s", err.c_str()); return false; }
        if (missing &&
            !SaveShortcut(path, SelfPath(), &err)) { LogErr("%s", err.c_str()); return false; }
    } else {
        if (legacy.empty()) return true;  // running the app is never consent to autostart
        // Migration must not silently undo a Task Manager disable.
        if (Blocked(kApprovedRun, kValue)) return true;
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(legacy.c_str(), &argc);
        if (argv && argc == 1) target = argv[0];
        if (argv) LocalFree(argv);
        if (target.empty()) {
            LogErr("autostart: cannot migrate a legacy Run command with arguments");
            return false;
        }
        bool missing = false;
        if (!TargetMissing(target, &missing, &err)) { LogErr("%s", err.c_str()); return false; }
        if (missing) target = SelfPath();
        if (!SaveShortcut(path, target, &err)) { LogErr("%s", err.c_str()); return false; }
    }
    if (!legacy.empty() && !RemoveLegacy(&err)) { LogErr("%s", err.c_str()); return false; }
    return true;
}
}  // namespace nsp
