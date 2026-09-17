// nsp_settings.cpp — see nsp_settings.h.

#include "nsp_settings.h"

#include "nsp_common.h"

#include <shlobj.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.Collections.h>  // JsonObject's IMap methods (HasKey) live here
#include <winrt/Windows.Data.Json.h>

namespace nsp {
namespace {

using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValueType;

// Every read checks the key and its type first. Windows.Data.Json throws on a missing key or a
// wrong type, and a hand-edited file with one bad value should cost that one value, not the
// whole file.
JsonObject ChildObject(const JsonObject& parent, const wchar_t* key) {
    if (!parent || !parent.HasKey(key)) return nullptr;
    const auto v = parent.GetNamedValue(key);
    return v.ValueType() == JsonValueType::Object ? v.GetObject() : nullptr;
}

void ReadBool(const JsonObject& parent, const wchar_t* key, bool* out) {
    if (!parent || !parent.HasKey(key)) return;
    const auto v = parent.GetNamedValue(key);
    if (v.ValueType() == JsonValueType::Boolean) *out = v.GetBoolean();
}

void ReadLanguage(const JsonObject& parent, const wchar_t* key, UiLanguage* out) {
    if (!parent || !parent.HasKey(key)) return;
    const auto v = parent.GetNamedValue(key);
    if (v.ValueType() != JsonValueType::String) return;
    const winrt::hstring s = v.GetString();
    *out = (s == L"en") ? UiLanguage::kEnglish : (s == L"ru") ? UiLanguage::kRussian : UiLanguage::kAuto;
}

const char* LanguageKey(UiLanguage lang) {
    switch (lang) {
        case UiLanguage::kEnglish: return "en";
        case UiLanguage::kRussian: return "ru";
        default: return "auto";
    }
}

std::string ErrorText(const char* step, const std::wstring& path, DWORD error) {
    return std::string("settings: ") + step + " failed for " + Narrow(path) +
           ", GetLastError=" + std::to_string(error);
}

// A scanner that opens the freshly written file for a moment makes the create or the rename
// fail with a sharing violation. Retried briefly, that is a delay instead of a lost setting.
bool Transient(DWORD e) {
    return e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED || e == ERROR_LOCK_VIOLATION;
}
constexpr int kSaveTries = 5;
constexpr DWORD kSaveRetryMs = 40;

}  // namespace

std::wstring SettingsPath() {
    wchar_t* base = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        CoTaskMemFree(base);  // the API allocates even on failure
        return {};
    }
    std::wstring path = base;
    CoTaskMemFree(base);
    if (!path.empty() && path.back() != L'\\') path += L'\\';
    return path + L"Nova SilkPlay\\settings.json";
}

bool LoadSettingsFrom(const std::wstring& path, Settings* out, std::string* err) {
    if (!out) return false;
    *out = Settings{};
    if (path.empty()) {
        if (err) *err = "settings: no settings path (LOCALAPPDATA unresolved)";
        return false;
    }

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        // Only a file that is not there means "first run". A file that exists but cannot be
        // opened — held by a scanner, access denied — must not read as defaults: the next save
        // would then overwrite every choice in it with them.
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return true;
        if (err) *err = ErrorText("open", path, e);
        return false;
    }

    LARGE_INTEGER size{};
    std::string bytes;
    bool readOk = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0 &&
                  size.QuadPart <= 1024 * 1024;
    if (readOk) {
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        readOk = bytes.empty() ||
                 (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr) &&
                  got == bytes.size());
    }
    CloseHandle(file);
    if (!readOk) {
        if (err) *err = "settings: could not read " + Narrow(path) + " (or it is over 1 MB)";
        return false;
    }

    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF)
        bytes.erase(0, 3);

    const int wideLen = bytes.empty() ? 0
                                      : MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                                            static_cast<int>(bytes.size()), nullptr, 0);
    if (wideLen <= 0) {
        if (err) *err = "settings: " + Narrow(path) + " is empty or not UTF-8";
        return false;
    }
    std::wstring text(static_cast<size_t>(wideLen), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()),
                        text.data(), wideLen);

    try {
        JsonObject root = nullptr;
        if (!JsonObject::TryParse(text, root) || !root) {
            if (err) *err = "settings: " + Narrow(path) + " is not a JSON object";
            return false;
        }
        const JsonObject capture = ChildObject(root, L"capture");
        ReadBool(ChildObject(capture, L"browsers"), L"chrome", &out->captureChrome);
        const JsonObject general = ChildObject(root, L"general");
        ReadBool(general, L"fpsCounter", &out->showFpsCounter);
        ReadLanguage(general, L"language", &out->language);
        // capture.players.potplayer is written for the day the player path exists, and ignored
        // until then: nothing may switch on a path that is not there.
        out->capturePotPlayer = false;
    } catch (const winrt::hresult_error& e) {
        *out = Settings{};
        if (err) *err = "settings: parsing " + Narrow(path) + " threw " + winrt::to_string(e.message());
        return false;
    } catch (...) {
        *out = Settings{};
        if (err) *err = "settings: parsing " + Narrow(path) + " threw";
        return false;
    }
    return true;
}

bool LoadSettings(Settings* out, std::string* err) {
    return LoadSettingsFrom(SettingsPath(), out, err);
}

bool SaveSettingsTo(const std::wstring& path, const Settings& s, std::string* err) {
    if (path.empty()) {
        if (err) *err = "settings: no settings path (LOCALAPPDATA unresolved)";
        return false;
    }
    const size_t slash = path.find_last_of(L'\\');
    if (slash != std::wstring::npos) CreateDirectoryW(path.substr(0, slash).c_str(), nullptr);

    const auto b = [](bool v) { return v ? "true" : "false"; };
    std::string json;
    json += "{\n";
    json += "  \"schema\": 1,\n";
    json += "  \"capture\": {\n";
    json += "    \"browsers\": {\n";
    json += std::string("      \"chrome\": ") + b(s.captureChrome) + "\n";
    json += "    },\n";
    json += "    \"players\": {\n";
    json += std::string("      \"potplayer\": ") + b(s.capturePotPlayer) + "\n";
    json += "    }\n";
    json += "  },\n";
    json += "  \"general\": {\n";
    json += std::string("    \"fpsCounter\": ") + b(s.showFpsCounter) + ",\n";
    json += std::string("    \"language\": \"") + LanguageKey(s.language) + "\"\n";
    json += "  }\n";
    json += "}\n";

    // Write-then-rename: a crash or a full disk mid-write leaves the previous file intact rather
    // than a truncated one that would load as defaults.
    const std::wstring tmp = path + L".tmp";
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD e = ERROR_SUCCESS;
    for (int attempt = 0; attempt < kSaveTries; ++attempt) {
        if (attempt > 0) Sleep(kSaveRetryMs);
        file = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        e = GetLastError();
        if (!Transient(e)) break;
    }
    if (file == INVALID_HANDLE_VALUE) {
        if (err) *err = ErrorText("create", tmp, e);
        return false;
    }
    DWORD wrote = 0;
    const bool written = WriteFile(file, json.data(), static_cast<DWORD>(json.size()), &wrote, nullptr) &&
                         wrote == json.size() && FlushFileBuffers(file);
    if (!written) e = GetLastError();
    CloseHandle(file);
    if (!written) {
        if (err) *err = ErrorText("write", tmp, e);
        DeleteFileW(tmp.c_str());
        return false;
    }
    bool moved = false;
    for (int attempt = 0; attempt < kSaveTries && !moved; ++attempt) {
        if (attempt > 0) Sleep(kSaveRetryMs);
        moved = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        if (!moved) {
            e = GetLastError();
            if (!Transient(e)) break;
        }
    }
    if (!moved) {
        if (err) *err = ErrorText("replace", path, e);
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

bool SaveSettings(const Settings& s, std::string* err) {
    return SaveSettingsTo(SettingsPath(), s, err);
}

}  // namespace nsp
