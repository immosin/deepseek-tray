#include "http.h"

#include <windows.h>
#include <winhttp.h>

#include <string>

namespace {

std::string lastWinError(DWORD code)
{
    wchar_t *buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::string out;
    if (buf) {
        const int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
        if (len > 1) {
            out.resize(static_cast<size_t>(len - 1));
            WideCharToMultiByte(CP_UTF8, 0, buf, -1, &out[0], len, nullptr, nullptr);
        }
        LocalFree(buf);
    }
    if (out.empty())
        out = "error " + std::to_string(code);
    return out;
}

std::wstring toWide(const std::string &utf8)
{
    if (utf8.empty())
        return std::wstring();
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring out(static_cast<size_t>(len > 0 ? len - 1 : 0), L'\0');
    if (len > 1)
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &out[0], len);
    return out;
}

} // namespace

HttpResult httpGet(const std::string &url, const std::string &bearer)
{
    HttpResult res;

    const std::wstring wurl = toWide(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256]{};
    wchar_t path[2048]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;

    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        res.error = "WinHttpCrackUrl: " + lastWinError(GetLastError());
        return res;
    }

    HINTERNET session = WinHttpOpen(L"DeepSeekTray/0.4", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        session = WinHttpOpen(L"DeepSeekTray/0.4", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        res.error = "WinHttpOpen: " + lastWinError(GetLastError());
        return res;
    }

    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
    WinHttpSetTimeouts(session, 5000, 5000, 10000, 15000);

    HINTERNET connect = WinHttpConnect(session, host, uc.nPort, 0);
    if (!connect) {
        res.error = "WinHttpConnect: " + lastWinError(GetLastError());
        WinHttpCloseHandle(session);
        return res;
    }

    const DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        res.error = "WinHttpOpenRequest: " + lastWinError(GetLastError());
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return res;
    }

    std::wstring headers = L"Accept: application/json\r\n";
    if (!bearer.empty())
        headers += L"Authorization: Bearer " + toWide(bearer) + L"\r\n";
    WinHttpAddRequestHeaders(request, headers.c_str(), static_cast<DWORD>(-1),
                             WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        res.error = "WinHttpSendRequest: " + lastWinError(GetLastError());
    } else if (!WinHttpReceiveResponse(request, nullptr)) {
        res.error = "WinHttpReceiveResponse: " + lastWinError(GetLastError());
    } else {
        DWORD status = 0;
        DWORD size = sizeof(status);
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                 WINHTTP_NO_HEADER_INDEX)) {
            res.error = "WinHttpQueryHeaders: " + lastWinError(GetLastError());
        } else {
            res.status = static_cast<int>(status);
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(request, &avail) && avail > 0) {
                std::string chunk(avail, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(request, &chunk[0], avail, &read))
                    break;
                chunk.resize(read);
                res.body += chunk;
                if (res.body.size() > (1u << 20))
                    break;
            }
            res.ok = (status >= 200 && status < 300);
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return res;
}
