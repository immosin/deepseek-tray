// Uninstall.exe — удаление DeepSeekTray из профиля пользователя.
//
// Удаляет ярлыки, ключ «Программ и компонент», останавливает приложение
// и через cmd удаляет собственную папку (процесс к этому моменту уже завершён).
//
// Ключи: /S — без подтверждения.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <string>

#include "winutil.h"

namespace {

std::wstring g_log;

std::wstring OwnDir()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s = path;
    const size_t slash = s.find_last_of(L'\\');
    return slash == std::wstring::npos ? s : s.substr(0, slash);
}

bool IsSilent()
{
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool silent = false;
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            std::wstring a = argv[i];
            if (a == L"/S" || a == L"/s" || a == L"-S" || a == L"-s")
                silent = true;
        }
        LocalFree(argv);
    }
    return silent;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    g_log = std::wstring(tmp) + L"DeepSeekTrayUninstall.log";

    const bool silent = IsSilent();
    const std::wstring dir = OwnDir();

    if (!silent) {
        const std::wstring text =
            L"Удалить DeepSeekTray?\r\n\r\n"
            L"Приложение будет остановлено, ярлыки и запись в списке программ удалены.\r\n"
            L"Лог и настройки останутся в %APPDATA%\\DeepSeekTray.";
        const int r = MessageBoxW(nullptr, text.c_str(), L"Удаление DeepSeekTray",
                                  MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2);
        if (r != IDYES)
            return 0;
    }

    LogLine(g_log, L"uninstall dir: " + dir);

    KillByName(L"DeepSeekTray.exe");
    WaitProcessGone(L"DeepSeekTray.exe");

    const std::wstring startupDir = GetKnownFolder(CSIDL_STARTUP);
    if (!startupDir.empty())
        DeleteFileW((startupDir + L"\\DeepSeekTray.lnk").c_str());

    const std::wstring programs = GetKnownFolder(CSIDL_PROGRAMS);
    if (!programs.empty())
        DeleteFileW((programs + L"\\DeepSeekTray.lnk").c_str());

    RemoveUninstallKey();

    wchar_t own[MAX_PATH]{};
    GetModuleFileNameW(nullptr, own, MAX_PATH);

    // Сначала сами удаляем всё, что можем (кроме собственного exe).
    RemoveTree(dir, std::wstring(own));

    // Остаток (свой exe и пустая папка) добивает cmd после нашего выхода.
    std::wstring cmd = L"cmd.exe /c ping -n 2 127.0.0.1 >nul & del /f /q \"";
    cmd += own;
    cmd += L"\" & rmdir /s /q \"";
    cmd += dir;
    cmd += L"\"";
    SpawnHidden(cmd);

    LogLine(g_log, L"uninstall: scheduled cleanup");
    return 0;
}
