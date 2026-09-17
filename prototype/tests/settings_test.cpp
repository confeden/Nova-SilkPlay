// settings_test.cpp — nsp_settings load/save behaviour, in a throwaway temp directory.

#include <windows.h>
#include <shlobj.h>
#include <winrt/base.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "nsp_settings.h"

namespace nsp {
std::string Narrow(const std::wstring& w);
}

// Test helper: creates a temp directory, returns its path.
std::wstring GetTempTestDir() {
    wchar_t tempPath[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tempPath)) {
        return L"";
    }
    std::wstring dir = tempPath;
    if (!dir.empty() && dir.back() != L'\\') dir += L'\\';

    // Add a unique suffix based on PID and timestamp.
    DWORD pid = GetCurrentProcessId();
    DWORD tick = GetTickCount();
    wchar_t suffix[64];
    swprintf_s(suffix, sizeof(suffix) / sizeof(suffix[0]), L"nsp_test_%u_%u", pid, tick);
    dir += suffix;

    if (!CreateDirectoryW(dir.c_str(), nullptr)) {
        return L"";
    }
    return dir;
}

// Test helper: recursively delete a directory.
bool DeleteDirRecursive(const std::wstring& path) {
    WIN32_FIND_DATAW ffd;
    HANDLE hFind = FindFirstFileW((path + L"\\*").c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE) {
        return false;
    }

    bool success = true;
    do {
        if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0) {
            continue;
        }
        std::wstring fullPath = path + L"\\" + ffd.cFileName;
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!DeleteDirRecursive(fullPath)) success = false;
        } else {
            if (!DeleteFileW(fullPath.c_str())) success = false;
        }
    } while (FindNextFileW(hFind, &ffd));

    FindClose(hFind);
    if (!RemoveDirectoryW(path.c_str())) success = false;
    return success;
}

// Test helper: write raw bytes to a file.
bool WriteBytes(const std::wstring& path, const std::vector<uint8_t>& data) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    DWORD bytesWritten = 0;
    bool success = ::WriteFile(hFile, data.data(), static_cast<DWORD>(data.size()),
                                &bytesWritten, nullptr) &&
                   (bytesWritten == data.size());
    CloseHandle(hFile);
    return success;
}

// Test helper: read file as raw bytes.
bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& data) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize)) {
        CloseHandle(hFile);
        return false;
    }

    data.resize(static_cast<size_t>(fileSize.QuadPart));
    DWORD bytesRead = 0;
    bool success = ::ReadFile(hFile, data.data(), static_cast<DWORD>(data.size()),
                               &bytesRead, nullptr) &&
                   (bytesRead == fileSize.QuadPart);
    CloseHandle(hFile);
    return success;
}

int main() {
    // Initialize COM in MTA mode.
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        printf("FAIL CoInitializeEx: COM initialization failed\n");
        return 1;
    }

    // Create a temp directory for testing.
    std::wstring testDir = GetTempTestDir();
    if (testDir.empty()) {
        printf("FAIL GetTempTestDir: failed to create temp directory\n");
        CoUninitialize();
        return 1;
    }

    int passCount = 0;
    int failCount = 0;

    // Test 1: missing file -> true, defaults
    {
        std::wstring path = testDir + L"\\test1.json";
        nsp::Settings s;
        std::string err;
        if (nsp::LoadSettingsFrom(path, &s, &err) && s.captureChrome && !s.capturePotPlayer &&
            s.showFpsCounter && s.language == nsp::UiLanguage::kAuto) {
            printf("PASS test1_missing_file\n");
            passCount++;
        } else {
            printf("FAIL test1_missing_file: expected defaults\n");
            failCount++;
        }
    }

    // Test 2: save non-defaults, load -> equal
    {
        std::wstring path = testDir + L"\\test2.json";
        nsp::Settings s1;
        s1.captureChrome = false;
        s1.showFpsCounter = false;
        s1.language = nsp::UiLanguage::kRussian;
        std::string err;

        if (!nsp::SaveSettingsTo(path, s1, &err)) {
            printf("FAIL test2_save_load: save failed: %s\n", err.c_str());
            failCount++;
        } else {
            nsp::Settings s2;
            if (!nsp::LoadSettingsFrom(path, &s2, &err)) {
                printf("FAIL test2_save_load: load failed: %s\n", err.c_str());
                failCount++;
            } else if (s1 == s2) {
                printf("PASS test2_save_load\n");
                passCount++;
            } else {
                printf("FAIL test2_save_load: loaded settings don't match\n");
                failCount++;
            }
        }
    }

    // Test 3: saved file contains "language": "ru" and no BOM
    {
        std::wstring path = testDir + L"\\test3.json";
        nsp::Settings s;
        s.language = nsp::UiLanguage::kRussian;
        std::string err;

        if (!nsp::SaveSettingsTo(path, s, &err)) {
            printf("FAIL test3_ru_no_bom: save failed\n");
            failCount++;
        } else {
            std::vector<uint8_t> data;
            if (!ReadFileBytes(path, data)) {
                printf("FAIL test3_ru_no_bom: read failed\n");
                failCount++;
            } else {
                // Check no BOM.
                bool noBom = !(data.size() >= 3 && data[0] == 0xEF && data[1] == 0xBB &&
                               data[2] == 0xBF);
                // Check first byte is {.
                bool startsWithBrace = !data.empty() && data[0] == '{';
                // Check contains "language": "ru".
                std::string content(reinterpret_cast<const char*>(data.data()),
                                     data.size());
                bool hasRu = content.find("\"language\": \"ru\"") != std::string::npos;

                if (noBom && startsWithBrace && hasRu) {
                    printf("PASS test3_ru_no_bom\n");
                    passCount++;
                } else {
                    printf("FAIL test3_ru_no_bom: noBom=%d startsWithBrace=%d hasRu=%d\n",
                           noBom, startsWithBrace, hasRu);
                    failCount++;
                }
            }
        }
    }

    // Test 4: file with potplayer: true -> loads with false
    {
        std::wstring path = testDir + L"\\test4.json";
        std::string jsonStr =
            "{\n"
            "  \"schema\": 1,\n"
            "  \"capture\": {\n"
            "    \"browsers\": { \"chrome\": true },\n"
            "    \"players\": { \"potplayer\": true }\n"
            "  },\n"
            "  \"general\": { \"fpsCounter\": true, \"language\": \"auto\" }\n"
            "}\n";
        std::vector<uint8_t> data(jsonStr.begin(), jsonStr.end());
        if (!WriteBytes(path, data)) {
            printf("FAIL test4_potplayer_forced_false: write failed\n");
            failCount++;
        } else {
            nsp::Settings s;
            std::string err;
            if (nsp::LoadSettingsFrom(path, &s, &err) && s.capturePotPlayer == false) {
                printf("PASS test4_potplayer_forced_false\n");
                passCount++;
            } else {
                printf("FAIL test4_potplayer_forced_false: potplayer not forced to false\n");
                failCount++;
            }
        }
    }

    // Test 5: wrong JSON types -> true, defaults
    {
        std::wstring path = testDir + L"\\test5.json";
        std::string jsonStr =
            "{\n"
            "  \"general\": { \"fpsCounter\": \"yes\", \"language\": 5 }\n"
            "}\n";
        std::vector<uint8_t> data(jsonStr.begin(), jsonStr.end());
        if (!WriteBytes(path, data)) {
            printf("FAIL test5_wrong_types: write failed\n");
            failCount++;
        } else {
            nsp::Settings s;
            std::string err;
            if (nsp::LoadSettingsFrom(path, &s, &err) && s.showFpsCounter == true &&
                s.language == nsp::UiLanguage::kAuto) {
                printf("PASS test5_wrong_types\n");
                passCount++;
            } else {
                printf("FAIL test5_wrong_types: didn't keep defaults for wrong types\n");
                failCount++;
            }
        }
    }

    // Test 6: invalid JSON -> false, error non-empty
    {
        std::wstring path = testDir + L"\\test6.json";
        std::string jsonStr = "not json";
        std::vector<uint8_t> data(jsonStr.begin(), jsonStr.end());
        if (!WriteBytes(path, data)) {
            printf("FAIL test6_invalid_json: write failed\n");
            failCount++;
        } else {
            nsp::Settings s;
            std::string err;
            if (!nsp::LoadSettingsFrom(path, &s, &err) && !err.empty()) {
                printf("PASS test6_invalid_json\n");
                passCount++;
            } else {
                printf("FAIL test6_invalid_json: should return false with error\n");
                failCount++;
            }
        }
    }

    // Test 7: UTF-8 BOM -> true, values read
    {
        std::wstring path = testDir + L"\\test7.json";
        std::vector<uint8_t> data = {0xEF, 0xBB, 0xBF};  // UTF-8 BOM
        std::string jsonStr =
            "{\n"
            "  \"capture\": { \"browsers\": { \"chrome\": false } },\n"
            "  \"general\": { \"fpsCounter\": false, \"language\": \"en\" }\n"
            "}\n";
        data.insert(data.end(), jsonStr.begin(), jsonStr.end());
        if (!WriteBytes(path, data)) {
            printf("FAIL test7_utf8_bom: write failed\n");
            failCount++;
        } else {
            nsp::Settings s;
            std::string err;
            if (nsp::LoadSettingsFrom(path, &s, &err) && !s.captureChrome &&
                !s.showFpsCounter && s.language == nsp::UiLanguage::kEnglish) {
                printf("PASS test7_utf8_bom\n");
                passCount++;
            } else {
                printf("FAIL test7_utf8_bom: failed to parse with BOM\n");
                failCount++;
            }
        }
    }

    // Test 8: unknown keys, missing capture -> true, rest read
    {
        std::wstring path = testDir + L"\\test8.json";
        std::string jsonStr =
            "{\n"
            "  \"unknownKey\": \"value\",\n"
            "  \"general\": { \"fpsCounter\": false, \"language\": \"en\" }\n"
            "}\n";
        std::vector<uint8_t> data(jsonStr.begin(), jsonStr.end());
        if (!WriteBytes(path, data)) {
            printf("FAIL test8_unknown_missing: write failed\n");
            failCount++;
        } else {
            nsp::Settings s;
            std::string err;
            if (nsp::LoadSettingsFrom(path, &s, &err) && !s.showFpsCounter &&
                s.language == nsp::UiLanguage::kEnglish &&
                s.captureChrome) {  // default
                printf("PASS test8_unknown_missing\n");
                passCount++;
            } else {
                printf("FAIL test8_unknown_missing: unexpected result\n");
                failCount++;
            }
        }
    }

    // Test 9: save twice, no .tmp left behind
    {
        std::wstring path = testDir + L"\\test9.json";
        nsp::Settings s1;
        s1.captureChrome = false;
        std::string err;

        if (!nsp::SaveSettingsTo(path, s1, &err)) {
            printf("FAIL test9_save_twice: first save failed\n");
            failCount++;
        } else {
            nsp::Settings s2;
            s2.captureChrome = true;
            if (!nsp::SaveSettingsTo(path, s2, &err)) {
                printf("FAIL test9_save_twice: second save failed\n");
                failCount++;
            } else {
                std::wstring tmpPath = path + L".tmp";
                nsp::Settings back;
                back.captureChrome = false;
                if (GetFileAttributesW(tmpPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    printf("FAIL test9_save_twice: .tmp file left behind\n");
                    failCount++;
                } else if (!nsp::LoadSettingsFrom(path, &back, &err) || !(back == s2)) {
                    printf("FAIL test9_save_twice: the second save did not replace the first\n");
                    failCount++;
                } else {
                    printf("PASS test9_save_twice\n");
                    passCount++;
                }
            }
        }
    }

    // Test 10: SettingsPath ends with \Nova SilkPlay\settings.json
    {
        std::wstring path = nsp::SettingsPath();
        // Length of "\Nova SilkPlay\settings.json" is 28 characters
        bool endsCorrectly =
            path.size() >= 28 &&
            path.substr(path.size() - 28) == L"\\Nova SilkPlay\\settings.json";
        if (endsCorrectly) {
            printf("PASS test10_settings_path\n");
            passCount++;
        } else {
            printf("FAIL test10_settings_path: path=%ls\n", path.c_str());
            failCount++;
        }
    }

    // Test 11: a file that exists but cannot be opened must NOT load as defaults
    {
        std::wstring path = testDir + L"\\test11.json";
        nsp::Settings s1;
        s1.showFpsCounter = false;
        std::string err;
        if (!nsp::SaveSettingsTo(path, s1, &err)) {
            printf("FAIL test11_locked_file: save failed: %s\n", err.c_str());
            failCount++;
        } else {
            HANDLE lock = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
            nsp::Settings s;
            err.clear();
            const bool ok = nsp::LoadSettingsFrom(path, &s, &err);
            if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
            if (lock == INVALID_HANDLE_VALUE) {
                printf("FAIL test11_locked_file: could not lock the file\n");
                failCount++;
            } else if (!ok && !err.empty()) {
                printf("PASS test11_locked_file\n");
                passCount++;
            } else {
                printf("FAIL test11_locked_file: a locked file loaded as success (err='%s')\n",
                       err.c_str());
                failCount++;
            }
        }
    }

    // Test 12: an empty file is an error, not defaults
    {
        std::wstring path = testDir + L"\\test12.json";
        std::vector<uint8_t> none;
        nsp::Settings s;
        std::string err;
        if (!WriteBytes(path, none)) {
            printf("FAIL test12_empty_file: write failed\n");
            failCount++;
        } else if (!nsp::LoadSettingsFrom(path, &s, &err) && !err.empty() && s == nsp::Settings{}) {
            printf("PASS test12_empty_file\n");
            passCount++;
        } else {
            printf("FAIL test12_empty_file: expected false, an error and defaults\n");
            failCount++;
        }
    }

    // Test 13: a save that meets a short lock on the destination (a scanner) still lands
    {
        std::wstring path = testDir + L"\\test13.json";
        nsp::Settings s1;
        std::string err;
        if (!nsp::SaveSettingsTo(path, s1, &err)) {
            printf("FAIL test13_transient_lock: first save failed: %s\n", err.c_str());
            failCount++;
        } else {
            HANDLE lock = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
            HANDLE releaser = CreateThread(
                nullptr, 0,
                [](LPVOID h) -> DWORD {
                    Sleep(90);
                    CloseHandle(static_cast<HANDLE>(h));
                    return 0;
                },
                lock, 0, nullptr);
            nsp::Settings s2;
            s2.showFpsCounter = false;
            err.clear();
            const bool ok = nsp::SaveSettingsTo(path, s2, &err);
            if (releaser) {
                WaitForSingleObject(releaser, INFINITE);
                CloseHandle(releaser);
            }
            nsp::Settings back;
            if (lock == INVALID_HANDLE_VALUE || !releaser) {
                printf("FAIL test13_transient_lock: could not set up the lock\n");
                failCount++;
            } else if (ok && nsp::LoadSettingsFrom(path, &back, &err) && back == s2) {
                printf("PASS test13_transient_lock\n");
                passCount++;
            } else {
                printf("FAIL test13_transient_lock: ok=%d err='%s'\n", ok, err.c_str());
                failCount++;
            }
        }
    }

    // Clean up temp directory.
    DeleteDirRecursive(testDir);

    // Cleanup COM.
    CoUninitialize();

    // Report results and exit.
    printf("\nResults: %d PASS, %d FAIL\n", passCount, failCount);
    return (failCount == 0) ? 0 : 1;
}
