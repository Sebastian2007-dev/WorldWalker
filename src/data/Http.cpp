#include "data/Http.h"

#include <windows.h>
#include <winhttp.h>

#include <mutex>

namespace net {
namespace {

HINTERNET session() {
    static HINTERNET s = [] {
        HINTERNET h = WinHttpOpen(L"WorldWalker/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        WinHttpSetOption(h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
        return h;
    }();
    return s;
}

std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

HttpResult getOnce(const std::string& url, int64_t b, int64_t e) {
    HttpResult res;
    std::wstring wurl = widen(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256], path[2048];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return res;

    HINTERNET con = WinHttpConnect(session(), host, uc.nPort, 0);
    if (!con) return res;
    DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(con, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (req) {
        std::wstring hdr;
        if (b >= 0) hdr = L"Range: bytes=" + std::to_wstring(b) + L"-" + std::to_wstring(e) + L"\r\n";
        if (WinHttpSendRequest(req, hdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdr.c_str(),
                               hdr.empty() ? 0 : (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            DWORD status = 0, sz = sizeof(status);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
            if (b >= 0 && e >= b) res.body.reserve(size_t(e - b + 1));
            bool readOk = true;
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(req, &avail)) { readOk = false; break; }
                if (avail == 0) break;
                size_t old = res.body.size();
                res.body.resize(old + avail);
                DWORD got = 0;
                if (!WinHttpReadData(req, res.body.data() + old, avail, &got)) { readOk = false; break; }
                res.body.resize(old + got);
            }
            res.status = readOk ? int(status) : 0;
        }
        WinHttpCloseHandle(req);
    }
    WinHttpCloseHandle(con);
    return res;
}

}  // namespace

HttpResult get(const std::string& url, int64_t rangeBegin, int64_t rangeEnd) {
    HttpResult r;
    for (int attempt = 0; attempt < 3; ++attempt) {
        r = getOnce(url, rangeBegin, rangeEnd);
        if (r.ok() || r.notFound()) return r;
        Sleep(200 * (attempt + 1));
    }
    return r;
}

}  // namespace net
