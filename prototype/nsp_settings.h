// nsp_settings.h — what the user chose in the Settings window, and where it is kept.
//
// Plain values with no behaviour: the tray thread edits a copy, the engine thread reads a copy,
// and neither ever holds a reference into the other's. The file is JSON under
// %LOCALAPPDATA%\Nova SilkPlay\ (kb/architecture.md section 6), written by hand and read with
// Windows.Data.Json, so a hand-edited file with one bad value loses only that value.

#pragma once

#include <string>

namespace nsp {

enum class UiLanguage { kAuto = 0, kEnglish = 1, kRussian = 2 };

struct Settings {
    // Capture targets. Browsers first; players are listed in the UI but not available yet.
    bool captureChrome = true;
    bool capturePotPlayer = false;  // the player path does not exist yet: always false after a load
    // General
    bool showFpsCounter = true;
    UiLanguage language = UiLanguage::kAuto;

    bool operator==(const Settings&) const = default;
};

// %LOCALAPPDATA%\Nova SilkPlay\settings.json  (SHGetKnownFolderPath(FOLDERID_LocalAppData)).
// Empty string if the known folder cannot be resolved.
std::wstring SettingsPath();

// Missing file -> *out = defaults, returns true, err untouched.
// Unreadable, not UTF-8, not a JSON object, or larger than 1 MB -> *out = defaults, returns false,
// *err = a short English reason that includes the path (narrowed with nsp::Narrow).
// Missing keys or keys of the wrong JSON type keep their default; unknown keys are ignored.
// capturePotPlayer is forced to false whatever the file says.
bool LoadSettingsFrom(const std::wstring& path, Settings* out, std::string* err);
bool LoadSettings(Settings* out, std::string* err);  // LoadSettingsFrom(SettingsPath(), ...)

// Creates the parent directory if needed (one level is enough), writes `<path>.tmp`
// (UTF-8, no BOM, CreateFileW + WriteFile + FlushFileBuffers), then
// MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH).
// On failure deletes the .tmp, returns false and fills *err with the step and GetLastError.
bool SaveSettingsTo(const std::wstring& path, const Settings& s, std::string* err);
bool SaveSettings(const Settings& s, std::string* err);  // SaveSettingsTo(SettingsPath(), ...)

}  // namespace nsp
