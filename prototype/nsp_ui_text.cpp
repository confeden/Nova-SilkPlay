// nsp_ui_text.cpp — see nsp_ui_text.h. Built with /utf-8: the literals below are UTF-8 source.

#include "nsp_ui_text.h"

#include "nsp_common.h"

#include <atomic>

namespace nsp {
namespace {

struct Entry {
    const wchar_t* en;
    const wchar_t* ru;
};

// Indexed by Txt. The formats of both languages take the same arguments in the same order.
constexpr Entry kText[] = {
    {L"Settings…", L"Настройки…"},
    {L"Frame generation\tCtrl+Alt+Q", L"Генерация кадров\tCtrl+Alt+Q"},
    {L"Cycle render mode (debug)\tCtrl+Alt+M", L"Сменить режим (отладка)\tCtrl+Alt+M"},
    {L"Quit\tCtrl+Alt+X", L"Выход\tCtrl+Alt+X"},
    {L"Nova SilkPlay — Settings", L"Nova SilkPlay — Настройки"},
    {L"Version %ls · build %ls", L"Версия %ls · сборка %ls"},
    {L"Browsers", L"Браузеры"},
    {L"Google Chrome", L"Google Chrome"},
    {L"Fullscreen and windowed video", L"Видео во весь экран и в окне"},
    {L"Players", L"Плееры"},
    {L"PotPlayer", L"PotPlayer"},
    {L"Coming in a future update", L"Появится в следующем обновлении"},
    {L"General", L"Общие"},
    {L"Start with Windows", L"Запускать вместе с Windows"},
    {L"Nova SilkPlay appears in the notification area at sign-in",
     L"Nova SilkPlay появляется в области уведомлений при входе"},
    {L"Turned off in Task Manager → Startup apps",
     L"Отключено в Диспетчере задач → Автозагрузка"},
    {L"Frame rate counter", L"Счётчик кадров"},
    {L"Source and output frame rate in the corner of the video",
     L"Исходная и итоговая частота в углу видео"},
    {L"Language", L"Язык"},
    {L"Tray menu and this window", L"Меню в трее и это окно"},
    {L"Same as Windows (%ls)", L"Как в Windows (%ls)"},
    {L"Ctrl+Alt+Q — turn frame generation on or off",
     L"Ctrl+Alt+Q — включить или выключить генерацию кадров"},
    {L"Ctrl+Alt+X — quit", L"Ctrl+Alt+X — выйти из программы"},
    {L"frame generation is off", L"генерация кадров выключена"},
    {L"no browser selected in Settings", L"в настройках не выбран браузер"},
    {L"waiting for a video in %ls", L"жду видео в %ls"},
    {L"paused: a window covers the video", L"пауза: видео закрыто окном"},
    {L"paused: a game is running (%ls)", L"пауза: запущена игра (%ls)"},
    {L"%ls: no video playing", L"%ls: видео не воспроизводится"},
    {L"%d → %d fps in %ls", L"%d → %d кадров/с в %ls"},
    {L"settings could not be read — choose a browser in Settings",
     L"настройки не прочитаны — выберите браузер в Настройках"},
    {L"Nova SilkPlay", L"Nova SilkPlay"},
    {L"The change could not be saved and applies to this session only.",
     L"Изменение не сохранено и действует только до выхода."},
    {L"protected video is not touched", L"защищённое видео не трогаем"},
};

std::atomic<bool> g_russian{false};

bool SystemIsRussian() {
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
}

}  // namespace

void SetUiLanguage(UiLanguage lang) {
    const bool ru = lang == UiLanguage::kRussian || (lang == UiLanguage::kAuto && SystemIsRussian());
    g_russian.store(ru, std::memory_order_relaxed);
}

bool UiIsRussian() { return g_russian.load(std::memory_order_relaxed); }

const wchar_t* SystemLanguageName() { return SystemIsRussian() ? L"Русский" : L"English"; }

const wchar_t* Tr(Txt id) {
    static_assert(ARRAYSIZE(kText) == static_cast<size_t>(Txt::kCount), "one entry per Txt");
    const auto& e = kText[static_cast<size_t>(id)];
    return UiIsRussian() ? e.ru : e.en;
}

}  // namespace nsp
