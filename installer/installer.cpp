// DeepSeekTraySetup — установщик без прав администратора.
//
// Кладёт файлы в %LOCALAPPDATA%\DeepSeekTray, создаёт ярлыки (Startup и меню «Пуск»),
// запись в «Программы и компоненты» и запускает приложение.
//
// Ключи: /S (тихая установка), /dir=<путь>, /startup=<папка>, /log=<файл>,
//        /ns (без автозапуска), /nr (без записи в реестре), /nl (не запускать).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <cwchar>

#include "payload_index.h"
#include "winutil.h"

#pragma comment(lib, "comctl32.lib")

namespace {

constexpr int ID_INSTALL = 100;
constexpr int ID_CANCEL = 101;
constexpr int ID_BROWSE = 102;
constexpr int ID_CHKAUTO = 103;
constexpr int ID_CHKRUN = 104;

constexpr int kClientW = 480;
constexpr int kClientH = 330;

HWND g_hwnd = nullptr;
HWND g_editPath = nullptr;
HWND g_status = nullptr;
HWND g_progress = nullptr;
HWND g_btnInstall = nullptr;
HWND g_btnCancel = nullptr;
HWND g_chkAuto = nullptr;
HWND g_chkRun = nullptr;
HFONT g_font = nullptr;
HFONT g_fontTitle = nullptr;

bool g_silent = false;
bool g_installing = false;
bool g_done = false;
std::wstring g_log;
std::wstring g_fallbackNote;

struct Options {
    std::wstring dir;
    std::wstring startup;
    std::wstring log;
    bool silent = false;
    bool noStartup = false;
    bool noReg = false;
    bool noLaunch = false;
    bool autoStart = true;
    bool runAfter = true;
    bool elevated = false;
};

std::wstring DefaultTarget()
{
    const std::wstring base = GetKnownFolder(CSIDL_LOCAL_APPDATA);
    if (base.empty())
        return L"C:\\DeepSeekTray";
    return base + L"\\DeepSeekTray";
}

Options ParseOptions()
{
    Options o;
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return o;

    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a.empty())
            continue;
        if (a[0] == L'/' || a[0] == L'-')
            a = a.substr(1);

        auto valueOf = [&](const wchar_t *prefix) -> std::wstring {
            const size_t n = wcslen(prefix);
            if (_wcsnicmp(a.c_str(), prefix, n) == 0)
                return a.substr(n);
            return std::wstring();
        };

        std::wstring v;
        if (!(v = valueOf(L"dir=")).empty())
            o.dir = v;
        else if (!(v = valueOf(L"startup=")).empty())
            o.startup = v;
        else if (!(v = valueOf(L"log=")).empty())
            o.log = v;
        else if (_wcsicmp(a.c_str(), L"s") == 0)
            o.silent = true;
        else if (_wcsicmp(a.c_str(), L"ns") == 0) {
            o.noStartup = true;
            o.autoStart = false;
        } else if (_wcsicmp(a.c_str(), L"nr") == 0)
            o.noReg = true;
        else if (_wcsicmp(a.c_str(), L"nl") == 0)
            o.noLaunch = true;
        else if (_wcsicmp(a.c_str(), L"noautostart") == 0)
            o.autoStart = false;
        else if (_wcsicmp(a.c_str(), L"nolaunch") == 0)
            o.runAfter = false;
        else if (_wcsicmp(a.c_str(), L"elevated") == 0)
            o.elevated = true;
    }
    LocalFree(argv);
    return o;
}

void SetStatus(const std::wstring &text)
{
    if (g_status)
        SetWindowTextW(g_status, text.c_str());
    LogLine(g_log, text);
}

void SetProgress(int done, int total)
{
    if (g_progress && total > 0)
        SendMessageW(g_progress, PBM_SETPOS, static_cast<WPARAM>(done * 100 / total), 0);
}

bool InstallFiles(const std::wstring &dir, std::wstring &err)
{
    std::wstring dirErr;
    if (!EnsureDirEx(dir, &dirErr)) {
        err = L"Не удалось создать папку:\r\n" + dir + L"\r\n\r\n" + dirErr;
        return false;
    }

    for (int i = 0; i < kPayloadCount; ++i) {
        const std::wstring rel = kPayload[i].path;
        const std::wstring full = dir + L"\\" + rel;

        const size_t slash = full.find_last_of(L'\\');
        if (slash != std::wstring::npos && !EnsureDirEx(full.substr(0, slash), &dirErr)) {
            err = L"Не удалось создать папку для " + rel + L": " + dirErr;
            return false;
        }

        HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(kPayload[i].id), RT_RCDATA);
        if (!res) {
            err = L"В установщике нет файла: " + rel;
            return false;
        }
        const DWORD size = SizeofResource(nullptr, res);
        HGLOBAL handle = LoadResource(nullptr, res);
        const void *data = handle ? LockResource(handle) : nullptr;
        if (!data) {
            err = L"Не удалось прочитать ресурс: " + rel;
            return false;
        }
        if (!WriteAllBytes(full, data, size)) {
            err = L"Не удалось записать: " + full;
            return false;
        }

        SetStatus(L"Копирование: " + rel);
        SetProgress(i + 1, kPayloadCount);
    }
    return true;
}

bool Install(const Options &o, std::wstring &err, std::wstring &installedDir)
{
    std::wstring dir = o.dir;
    std::wstring dirErr;
    LogLine(g_log, L"target: " + dir);
    {
        std::wstring diag = TokenDiagnostics();
        for (wchar_t &ch : diag) {
            if (ch == L'\r' || ch == L'\n')
                ch = L' ';
        }
        LogLine(g_log, L"env: " + diag);
    }

    if (!EnsureDirEx(dir, &dirErr)) {
        LogLine(g_log, L"mkdir failed for " + dir + L": " + dirErr);

        // Пользователь выбрал папку вручную — не подменяем её молча.
        if (dir == DefaultTarget()) {
            const std::wstring local = GetKnownFolder(CSIDL_LOCAL_APPDATA);
            const std::wstring roaming = GetKnownFolder(CSIDL_APPDATA);
            const std::wstring publicDir = GetKnownFolder(CSIDL_COMMON_DOCUMENTS);
            const std::wstring candidates[] = {
                local + L"\\Programs\\DeepSeekTray",
                roaming + L"\\DeepSeekTray",
                publicDir + L"\\DeepSeekTray",
            };

            for (const std::wstring &cand : candidates) {
                std::wstring candErr;
                if (cand.size() < 4)
                    continue;
                if (EnsureDirEx(cand, &candErr)) {
                    LogLine(g_log, L"fallback dir: " + cand);
                    g_fallbackNote = L"Папка по умолчанию недоступна (" + dirErr +
                                     L").\r\n\r\nУстановлено в:\r\n" + cand +
                                     L"\r\n\r\nЕсли ярлыки или автозапуск не появились — "
                                     L"запустите установщик из Проводника.";
                    dir = cand;
                    dirErr.clear();
                    break;
                }
                LogLine(g_log, L"fallback failed: " + cand + L": " + candErr);
            }
        }

        if (!dirErr.empty()) {
            err = L"Не удалось создать папку:\r\n" + o.dir + L"\r\n\r\n" + dirErr +
                  L"\r\n\r\nВыберите другую папку кнопкой «Обзор…».\r\n"
                  L"Если установщик запущен из окна DeepSeek Harness, закройте его и "
                  L"запустите DeepSeekTraySetup.exe двойным щелчком в Проводнике."
                  L"\r\n\r\n" + TokenDiagnostics();
            return false;
        }
    }

    KillByName(L"DeepSeekTray.exe");
    WaitProcessGone(L"DeepSeekTray.exe");
    Sleep(100);

    if (!InstallFiles(dir, err))
        return false;

    const std::wstring exe = dir + L"\\DeepSeekTray.exe";

    if (!o.noStartup) {
        const std::wstring startupDir =
            o.startup.empty() ? GetKnownFolder(CSIDL_STARTUP) : o.startup;
        if (!startupDir.empty()) {
            std::wstring lgErr;
            EnsureDirEx(startupDir, &lgErr);
            const std::wstring lnk = startupDir + L"\\DeepSeekTray.lnk";
            if (o.autoStart) {
                if (!CreateShortcutW(lnk, exe, dir, L"DeepSeek API balance in tray"))
                    LogLine(g_log, L"WARN: не удалось создать ярлык автозапуска: " + lnk + L" (" + lgErr + L")");
                else
                    LogLine(g_log, L"autostart: " + lnk);
            } else {
                DeleteFileW(lnk.c_str());
            }
        }

        const std::wstring programs = GetKnownFolder(CSIDL_PROGRAMS);
        if (!programs.empty()) {
            std::wstring lgErr;
            EnsureDirEx(programs, &lgErr);
            const std::wstring lnk = programs + L"\\DeepSeekTray.lnk";
            if (!CreateShortcutW(lnk, exe, dir, L"DeepSeek API balance in tray"))
                LogLine(g_log, L"WARN: не удалось создать ярлык меню Пуск: " + lnk + L" (" + lgErr + L")");
            else
                LogLine(g_log, L"start menu: " + lnk);
        }
    }

    if (!o.noReg) {
        if (!WriteUninstallKey(dir))
            LogLine(g_log, L"WARN: не удалось записать ключ удаления");
        else
            LogLine(g_log, L"uninstall key: ok");
    }

    if (!o.noLaunch && o.runAfter) {
        HINSTANCE r = ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
        LogLine(g_log, std::wstring(L"launch: ") + (reinterpret_cast<INT_PTR>(r) > 32 ? L"ok" : L"fail"));
    }

    LogLine(g_log, L"install: ok");
    installedDir = dir;
    return true;
}

void CreateFonts()
{
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);
        LOGFONTW bold = ncm.lfMessageFont;
        bold.lfWeight = FW_SEMIBOLD;
        g_fontTitle = CreateFontIndirectW(&bold);
    }
    if (!g_font)
        g_font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    if (!g_fontTitle)
        g_fontTitle = g_font;
}

HWND MakeControl(const wchar_t *cls, const wchar_t *text, DWORD style,
                 int x, int y, int w, int h, int id, HFONT font)
{
    HWND hwnd = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                                x, y, w, h, g_hwnd,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                GetModuleHandleW(nullptr), nullptr);
    if (hwnd && font)
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return hwnd;
}

void BrowseForFolder()
{
    BROWSEINFOW bi{};
    wchar_t display[MAX_PATH]{};
    bi.hwndOwner = g_hwnd;
    bi.lpszTitle = L"Куда установить DeepSeekTray";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.pszDisplayName = display;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl)
        return;
    wchar_t path[MAX_PATH]{};
    if (SHGetPathFromIDListW(pidl, path))
        SetWindowTextW(g_editPath, path);
    CoTaskMemFree(pidl);
}

void DoInstall()
{
    Options o;
    {
        const int len = GetWindowTextLengthW(g_editPath);
        std::wstring buf(static_cast<size_t>(len) + 1, L'\0');
        GetWindowTextW(g_editPath, &buf[0], len + 1);
        buf.resize(static_cast<size_t>(len));
        o.dir = buf;
    }
    if (o.dir.empty())
        o.dir = DefaultTarget();

    o.autoStart = SendMessageW(g_chkAuto, BM_GETCHECK, 0, 0) == BST_CHECKED;
    o.runAfter = SendMessageW(g_chkRun, BM_GETCHECK, 0, 0) == BST_CHECKED;
    o.log = g_log;

    g_installing = true;
    EnableWindow(g_btnInstall, FALSE);
    EnableWindow(g_btnCancel, FALSE);
    EnableWindow(g_editPath, FALSE);
    EnableWindow(g_chkAuto, FALSE);
    EnableWindow(g_chkRun, FALSE);

    std::wstring err;
    std::wstring installedDir;
    const bool ok = Install(o, err, installedDir);

    g_installing = false;
    if (ok) {
        g_done = true;
        if (installedDir != o.dir)
            SetWindowTextW(g_editPath, installedDir.c_str());
        SetStatus(L"Готово. Установлено в " + installedDir);
        SetWindowTextW(g_btnInstall, L"Закрыть");
        EnableWindow(g_btnInstall, TRUE);
        EnableWindow(g_btnCancel, TRUE);
        ShowWindow(g_btnCancel, SW_HIDE);
        SetFocus(g_btnInstall);
        if (!g_fallbackNote.empty())
            MessageBoxW(g_hwnd, g_fallbackNote.c_str(), L"Установка завершена",
                        MB_ICONINFORMATION | MB_OK);
    } else {
        LogLine(g_log, L"install error: " + err);
        SetStatus(L"Ошибка: " + err.substr(0, 160));

        std::wstring msg = err;
        if (!o.elevated)
            msg += L"\r\n\r\nЗапустить установщик от имени администратора?";

        const UINT flags = MB_ICONERROR | (o.elevated ? MB_OK : MB_YESNO);
        const int choice = MessageBoxW(g_hwnd, msg.c_str(), L"Установка не удалась", flags);

        if (!o.elevated && choice == IDYES) {
            wchar_t self[MAX_PATH]{};
            GetModuleFileNameW(nullptr, self, MAX_PATH);
            const std::wstring args = L"/dir=\"" + o.dir + L"\" /elevated";
            const HINSTANCE r = ShellExecuteW(nullptr, L"runas", self, args.c_str(), nullptr, SW_SHOWNORMAL);
            LogLine(g_log, std::wstring(L"elevate: ") +
                               (reinterpret_cast<INT_PTR>(r) > 32 ? L"started" : L"failed"));
            if (reinterpret_cast<INT_PTR>(r) > 32) {
                DestroyWindow(g_hwnd);
                return;
            }
            MessageBoxW(g_hwnd, L"Не удалось запустить установщик с повышением прав.",
                        L"DeepSeekTray", MB_ICONWARNING | MB_OK);
        }

        EnableWindow(g_btnInstall, TRUE);
        EnableWindow(g_btnCancel, TRUE);
        EnableWindow(g_editPath, TRUE);
        EnableWindow(g_chkAuto, TRUE);
        EnableWindow(g_chkRun, TRUE);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
        CreateFonts();

        HWND title = MakeControl(L"STATIC", L"DeepSeekTray",
                                 SS_LEFT, 20, 16, 440, 26, 0, g_fontTitle);
        (void)title;

        MakeControl(L"STATIC",
                    L"Иконка с балансом DeepSeek API в системном трее.\r\n"
                    L"Устанавливается в профиль пользователя, права администратора не нужны.",
                    SS_LEFT, 20, 46, 440, 40, 0, g_font);

        MakeControl(L"STATIC", L"Папка установки:", SS_LEFT, 20, 92, 220, 18, 0, g_font);

        g_editPath = MakeControl(L"EDIT", DefaultTarget().c_str(),
                                 WS_BORDER | ES_AUTOHSCROLL, 20, 112, 340, 24, 0, g_font);
        MakeControl(L"BUTTON", L"Обзор…", BS_PUSHBUTTON, 368, 112, 88, 24, ID_BROWSE, g_font);

        g_chkAuto = MakeControl(L"BUTTON", L"Запускать при входе в систему",
                                BS_AUTOCHECKBOX, 20, 150, 440, 20, ID_CHKAUTO, g_font);
        SendMessageW(g_chkAuto, BM_SETCHECK, BST_CHECKED, 0);

        g_chkRun = MakeControl(L"BUTTON", L"Запустить после установки",
                               BS_AUTOCHECKBOX, 20, 174, 440, 20, ID_CHKRUN, g_font);
        SendMessageW(g_chkRun, BM_SETCHECK, BST_CHECKED, 0);

        g_progress = MakeControl(PROGRESS_CLASSW, L"", PBS_SMOOTH,
                                 20, 210, 438, 16, 0, g_font);
        SendMessageW(g_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

        g_status = MakeControl(L"STATIC", L"Готов к установке.", SS_LEFT, 20, 234, 438, 36, 0, g_font);

        g_btnInstall = MakeControl(L"BUTTON", L"Установить", BS_DEFPUSHBUTTON,
                                   266, 284, 96, 28, ID_INSTALL, g_font);
        g_btnCancel = MakeControl(L"BUTTON", L"Отмена", BS_PUSHBUTTON,
                                  370, 284, 88, 28, ID_CANCEL, g_font);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_INSTALL:
            if (g_done)
                DestroyWindow(hwnd);
            else if (!g_installing)
                DoInstall();
            return 0;
        case ID_CANCEL:
            if (!g_installing)
                DestroyWindow(hwnd);
            return 0;
        case ID_BROWSE:
            if (!g_installing)
                BrowseForFolder();
            return 0;
        default:
            break;
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
    }
    case WM_CLOSE:
        if (!g_installing)
            DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    Options o = ParseOptions();
    g_silent = o.silent;
    g_log = o.log;
    if (g_log.empty()) {
        wchar_t tmp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tmp);
        g_log = std::wstring(tmp) + L"DeepSeekTraySetup.log";
    }

    if (g_silent) {
        if (o.dir.empty())
            o.dir = DefaultTarget();
        std::wstring err;
        std::wstring installedDir;
        const bool ok = Install(o, err, installedDir);
        if (!ok)
            LogLine(g_log, L"install: FAIL " + err);
        else {
            LogLine(g_log, L"installed to: " + installedDir);
            if (!g_fallbackNote.empty())
                LogLine(g_log, L"note: " + g_fallbackNote);
        }
        CoUninitialize();
        return ok ? 0 : 1;
    }

    if (o.dir.empty())
        o.dir = DefaultTarget();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"DeepSeekTraySetupWnd";
    RegisterClassExW(&wc);

    RECT rc{0, 0, kClientW, kClientH};
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0);

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"Установка DeepSeekTray",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) {
        CoUninitialize();
        return 1;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    CoUninitialize();
    return 0;
}
