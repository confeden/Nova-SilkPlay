// Real ShellLink persistence, isolated from the user's Startup directory and registry.
#include "nsp_common.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

static std::wstring g_dir;
static std::map<std::wstring, std::vector<BYTE>> g_registry;
static bool g_deleteDenied = false;
static int g_failed = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++g_failed; } } while (0)

static HRESULT TestKnownFolder(REFKNOWNFOLDERID id, DWORD, HANDLE, PWSTR* out) {
    CHECK(IsEqualGUID(id, FOLDERID_Startup));
    const size_t cb = (g_dir.size() + 1) * sizeof(wchar_t);
    *out = static_cast<PWSTR>(CoTaskMemAlloc(cb));
    if (!*out) return E_OUTOFMEMORY;
    memcpy(*out, g_dir.c_str(), cb);
    return S_OK;
}
static LSTATUS TestRegGet(HKEY, LPCWSTR key, LPCWSTR name, DWORD, LPDWORD, PVOID data, LPDWORD cb) {
    const auto it = g_registry.find(std::wstring(key) + L"\\" + name);
    if (it == g_registry.end()) return ERROR_FILE_NOT_FOUND;
    if (*cb < it->second.size()) return ERROR_MORE_DATA;
    *cb = static_cast<DWORD>(it->second.size());
    memcpy(data, it->second.data(), *cb);
    return ERROR_SUCCESS;
}
static LSTATUS TestRegDelete(HKEY, LPCWSTR key, LPCWSTR name) {
    if (g_deleteDenied) return ERROR_ACCESS_DENIED;
    return g_registry.erase(std::wstring(key) + L"\\" + name) ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
}
static UINT TestTempFileName(LPCWSTR dir, LPCWSTR prefix, UINT unique, LPWSTR out) {
    std::wstring staging = dir;
    while (!staging.empty() && (staging.back() == L'\\' || staging.back() == L'/')) staging.pop_back();
    CHECK(staging != g_dir);
    return GetTempFileNameW(dir, prefix, unique, out);
}
#define GetTempFileNameW TestTempFileName
#define SHGetKnownFolderPath TestKnownFolder
#define RegGetValueW TestRegGet
#define RegDeleteKeyValueW TestRegDelete
#include "../nsp_autostart.cpp"
#undef SHGetKnownFolderPath
#undef GetTempFileNameW
#undef RegGetValueW
#undef RegDeleteKeyValueW

static void Legacy(const std::wstring& command) {
    const auto p = reinterpret_cast<const BYTE*>(command.c_str());
    g_registry[std::wstring(nsp::kRunKey) + L"\\" + nsp::kValue] =
        std::vector<BYTE>(p, p + (command.size() + 1) * sizeof(wchar_t));
}
static void Block(const wchar_t* key, const wchar_t* name, bool on) {
    g_registry[std::wstring(key) + L"\\" + name] = {static_cast<BYTE>(on ? 3 : 2)};
}

int main() {
    CHECK(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)));
    nsp::SetFileLogging(false);
    wchar_t tmp[MAX_PATH]{};
    CHECK(GetTempPathW(MAX_PATH, tmp) != 0);
    g_dir = std::wstring(tmp) + L"Nova SilkPlay startup test " + std::to_wstring(GetCurrentProcessId());
    CHECK(CreateDirectoryW(g_dir.c_str(), nullptr));
    const std::wstring link = g_dir + L"\\Nova SilkPlay.lnk";
    std::string err;
    std::wstring target;
    CHECK(nsp::AutostartQuery() == nsp::AutostartState::kOff);
    CHECK(nsp::AutostartRepair());
    CHECK(GetFileAttributesW(link.c_str()) == INVALID_FILE_ATTRIBUTES);
    CHECK(nsp::AutostartSet(true, &err));
    CHECK(nsp::AutostartQuery() == nsp::AutostartState::kOn);
    CHECK(nsp::LoadShortcut(link, &target, &err));
    CHECK(target == nsp::SelfPath());
    CHECK(nsp::AutostartSet(true, &err));  // atomic replacement, paths with spaces
    Block(nsp::kApprovedFolder, nsp::kShortcut, true);
    CHECK(nsp::AutostartQuery() == nsp::AutostartState::kBlocked);
    CHECK(nsp::AutostartSet(true, &err));
    CHECK(nsp::AutostartQuery() == nsp::AutostartState::kBlocked); // never fight Task Manager
    CHECK(nsp::AutostartSet(false, &err));
    CHECK(nsp::AutostartQuery() == nsp::AutostartState::kOff);
    CHECK(nsp::AutostartSet(false, &err));
    g_registry.clear();

    Legacy(L"\"" + nsp::SelfPath() + L"\"");
    CHECK(nsp::AutostartRepair());
    CHECK(nsp::ReadRunValue().empty());
    CHECK(nsp::LoadShortcut(link, &target, &err));
    CHECK(target == nsp::SelfPath());
    CHECK(nsp::AutostartSet(false, &err));
    Legacy(L"\"" + nsp::SelfPath() + L"\"");
    Block(nsp::kApprovedRun, nsp::kValue, true);
    CHECK(nsp::AutostartRepair());
    CHECK(nsp::AutostartQuery() == nsp::AutostartState::kBlocked);
    CHECK(GetFileAttributesW(link.c_str()) == INVALID_FILE_ATTRIBUTES);
    CHECK(nsp::AutostartSet(false, &err));
    g_registry.clear();

    const std::wstring other = g_dir + L"\\installed copy.exe";
    HANDLE file = CreateFileW(other.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(file != INVALID_HANDLE_VALUE);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    Legacy(L"\"" + other + L"\"");
    CHECK(nsp::AutostartRepair());
    CHECK(nsp::LoadShortcut(link, &target, &err));
    CHECK(target == other);  // migrate the installed target, not the development build
    CHECK(nsp::AutostartRepair());
    CHECK(nsp::LoadShortcut(link, &target, &err));
    CHECK(target == other);
    CHECK(DeleteFileW(other.c_str()));
    CHECK(nsp::AutostartRepair());
    CHECK(nsp::LoadShortcut(link, &target, &err));
    CHECK(target == nsp::SelfPath());
    CHECK(nsp::AutostartSet(false, &err));
    Legacy(L"\"" + nsp::SelfPath() + L"\" --unexpected");
    CHECK(!nsp::AutostartRepair());
    CHECK(!nsp::ReadRunValue().empty()); // do not silently drop arguments
    g_deleteDenied = true;
    CHECK(!nsp::AutostartSet(false, &err));
    CHECK(!err.empty());
    g_deleteDenied = false;
    CHECK(nsp::AutostartSet(false, &err));
    // A corrupt existing shortcut is reported, never silently replaced by Repair.
    file = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(file != INVALID_HANDLE_VALUE);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    CHECK(nsp::AutostartQuery() == nsp::AutostartState::kOff);
    CHECK(!nsp::AutostartRepair());
    CHECK(DeleteFileW(link.c_str()));
    const std::wstring savedDir = g_dir;
    g_dir += L"\\missing";
    err.clear();
    CHECK(!nsp::AutostartSet(true, &err));
    CHECK(!err.empty());
    g_dir = savedDir;
    CHECK(RemoveDirectoryW(g_dir.c_str()));
    CoUninitialize();
    printf("Autostart: %s (%d failed checks)\n", g_failed ? "FAIL" : "PASS", g_failed);
    return g_failed ? 1 : 0;
}
