#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <sddl.h>
#include <string>
#include <vector>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

inline std::wstring GetKnownFolder(int csidl)
{
    wchar_t buf[MAX_PATH]{};
    if (SHGetFolderPathW(nullptr, csidl | CSIDL_FLAG_CREATE, nullptr, SHGFP_TYPE_CURRENT, buf) != S_OK)
        return std::wstring();
    return buf;
}

inline std::wstring WinErrText(DWORD code)
{
    wchar_t *buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring text = buf ? buf : L"";
    if (buf)
        LocalFree(buf);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
        text.pop_back();
    return text;
}

inline std::wstring SidToString(PSID sid)
{
    LPWSTR text = nullptr;
    if (ConvertSidToStringSidW(sid, &text)) {
        std::wstring result = text;
        LocalFree(text);
        return result;
    }
    return L"?";
}

// Краткая информация о токене процесса — чтобы понять, почему Windows отказывает в записи.
inline std::wstring TokenDiagnostics()
{
    std::wstring out;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return L"токен: недоступен";

    DWORD elevation = 0;
    DWORD size = 0;
    if (GetTokenInformation(token, TokenElevationType, &elevation, sizeof(elevation), &size)) {
        out += L"elevation: ";
        out += (elevation == TokenElevationTypeFull    ? L"full"
                : elevation == TokenElevationTypeLimited ? L"limited"
                                                         : L"default");
        out += L"\r\n";
    }

    size = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
    if (size) {
        std::vector<BYTE> buf(size);
        if (GetTokenInformation(token, TokenIntegrityLevel, buf.data(), size, &size)) {
            auto *label = reinterpret_cast<TOKEN_MANDATORY_LABEL *>(buf.data());
            out += L"integrity: " + SidToString(label->Label.Sid) + L"\r\n";
        }
    }

    out += std::wstring(L"restricted: ") + (IsTokenRestricted(token) ? L"yes" : L"no") + L"\r\n";

    size = 0;
    GetTokenInformation(token, TokenAppContainerSid, nullptr, 0, &size);
    if (size) {
        std::vector<BYTE> buf(size);
        if (GetTokenInformation(token, TokenAppContainerSid, buf.data(), size, &size)) {
            auto sid = *reinterpret_cast<PSID *>(buf.data());
            if (sid)
                out += L"appcontainer: " + SidToString(sid) + L"\r\n";
        }
    }

    CloseHandle(token);
    return out;
}
// Создаёт каталог по шагам (без SHCreateDirectoryEx, у которого бывают свои причуды)
// и возвращает человекочитаемую причину отказа.
inline bool EnsureDirEx(const std::wstring &path, std::wstring *error)
{
    std::wstring p = path;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/'))
        p.pop_back();
    if (p.empty()) {
        if (error)
            *error = L"пустой путь";
        return false;
    }

    const DWORD attrs = GetFileAttributesW(p.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        if (attrs & FILE_ATTRIBUTE_DIRECTORY)
            return true;
        if (error)
            *error = L"по этому пути уже лежит файл";
        return false;
    }

    const size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 2) {
        if (!EnsureDirEx(p.substr(0, slash), error))
            return false;
    }

    if (CreateDirectoryW(p.c_str(), nullptr))
        return true;

    const DWORD code = GetLastError();
    if (code == ERROR_ALREADY_EXISTS)
        return true;

    if (error) {
        *error = L"код " + std::to_wstring(code);
        const std::wstring text = WinErrText(code);
        if (!text.empty())
            *error += L": " + text;
    }
    return false;
}

inline bool EnsureDir(const std::wstring &path)
{
    return EnsureDirEx(path, nullptr);
}

inline bool WriteAllBytes(const std::wstring &path, const void *data, DWORD size)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(h, data, size, &written, nullptr);
    CloseHandle(h);
    return ok && written == size;
}

inline bool CreateShortcutW(const std::wstring &lnk, const std::wstring &target,
                            const std::wstring &workDir, const std::wstring &desc)
{
    IShellLinkW *link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, reinterpret_cast<void **>(&link))))
        return false;

    link->SetPath(target.c_str());
    link->SetWorkingDirectory(workDir.c_str());
    link->SetDescription(desc.c_str());
    link->SetIconLocation(target.c_str(), 0);

    IPersistFile *file = nullptr;
    bool ok = false;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void **>(&file)))) {
        ok = SUCCEEDED(file->Save(lnk.c_str(), TRUE));
        file->Release();
    }
    link->Release();
    return ok;
}

inline bool SpawnHidden(const std::wstring &cmd, bool wait = false)
{
    std::wstring c = cmd;
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &c[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi))
        return false;
    if (wait) {
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

inline bool KillByName(const wchar_t *exeName)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool killed = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) != 0 || pe.th32ProcessID == GetCurrentProcessId())
                continue;
            HANDLE proc = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
            if (!proc)
                continue;
            if (TerminateProcess(proc, 0))
                killed = true;
            WaitForSingleObject(proc, 3000);
            CloseHandle(proc);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return killed;
}

inline bool ProcessRunning(const wchar_t *exeName)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

inline void WaitProcessGone(const wchar_t *exeName, DWORD timeoutMs = 5000)
{
    const DWORD step = 100;
    for (DWORD waited = 0; waited < timeoutMs; waited += step) {
        if (!ProcessRunning(exeName))
            return;
        Sleep(step);
    }
}

// Удаляет содержимое каталога, кроме keepPath (собственный exe удалить нельзя).
inline void RemoveTree(const std::wstring &dir, const std::wstring &keepPath)
{
    WIN32_FIND_DATAW fd{};
    const std::wstring mask = dir + L"\\*";
    HANDLE h = FindFirstFileW(mask.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..")
            continue;
        const std::wstring full = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            RemoveTree(full, keepPath);
            RemoveDirectoryW(full.c_str());
        } else if (_wcsicmp(full.c_str(), keepPath.c_str()) != 0) {
            DeleteFileW(full.c_str());
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

inline bool WriteUninstallKey(const std::wstring &dir)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\DeepSeekTray",
                        0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;

    auto setStr = [&](const wchar_t *name, const std::wstring &value) {
        RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    };

    setStr(L"DisplayName", L"DeepSeekTray");
    setStr(L"DisplayVersion", L"0.4.0");
    setStr(L"Publisher", L"DeepSeekTray");
    setStr(L"InstallLocation", dir);
    setStr(L"DisplayIcon", dir + L"\\DeepSeekTray.exe");
    setStr(L"UninstallString", L"\"" + dir + L"\\Uninstall.exe\"");
    setStr(L"QuietUninstallString", L"\"" + dir + L"\\Uninstall.exe\" /S");

    DWORD zero = 0;
    RegSetValueExW(key, L"NoModify", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&zero), sizeof(zero));
    RegSetValueExW(key, L"NoRepair", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&zero), sizeof(zero));
    RegCloseKey(key);
    return true;
}

inline void RemoveUninstallKey()
{
    RegDeleteTreeW(HKEY_CURRENT_USER,
                   L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\DeepSeekTray");
}

inline void LogLine(const std::wstring &path, const std::wstring &text)
{
    if (path.empty())
        return;
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(h, &size) && size.QuadPart == 0) {
        const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
        DWORD bomWritten = 0;
        WriteFile(h, bom, 3, &bomWritten, nullptr);
    }
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t prefix[64]{};
    wsprintfW(prefix, L"%04d-%02d-%02d %02d:%02d:%02d ",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring line = std::wstring(prefix) + text + L"\r\n";
    int len = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(len > 0 ? len - 1 : 0, '\0');
    if (len > 1)
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, &utf8[0], len, nullptr, nullptr);
    DWORD written = 0;
    WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(h);
}
