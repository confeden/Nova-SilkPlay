// nsp_ui_text.h — every string a user reads, in English and Russian.
//
// Log lines stay English: they are search keys for real program output. Only what appears in
// the tray (menu, tooltip) and in the Settings window goes through here.
#pragma once

#include "nsp_settings.h"

namespace nsp {

enum class Txt {
    kTraySettings,
    kTrayGeneration,
    kTrayCycleMode,
    kTrayQuit,
    kWindowTitle,
    kVersionLine,       // %ls version, %ls build
    kSectionBrowsers,
    kChromeTitle,
    kChromeNote,
    kSectionPlayers,
    kPotPlayerTitle,
    kPotPlayerNote,
    kSectionGeneral,
    kCounterTitle,
    kCounterNote,
    kLanguageTitle,
    kLanguageNote,
    kLanguageAuto,      // %ls the language "same as Windows" resolves to
    kHotkeyToggle,
    kHotkeyQuit,
    kStatusOff,
    kStatusNothingSelected,
    kStatusWaiting,     // %ls application
    kStatusCovered,
    kStatusNoVideo,     // %ls application
    kStatusGenerating,  // %d source fps, %d output fps, %ls application
    kStatusSettingsUnreadable,
    kCount
};

// kAuto follows the Windows display language: Russian if it is Russian, English otherwise.
// Any thread; the tray tooltip is built on the engine thread, the menu and window on the UI one.
void SetUiLanguage(UiLanguage lang);
bool UiIsRussian();
// The language kAuto resolves to on this machine, named in that language ("English", "Русский").
const wchar_t* SystemLanguageName();
const wchar_t* Tr(Txt id);

}  // namespace nsp
