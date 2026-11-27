#pragma once

#include <string>

struct HttpResult {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string error;
};

// Синхронный GET с Bearer-токеном через WinHTTP.
HttpResult httpGet(const std::string &url, const std::string &bearer);
