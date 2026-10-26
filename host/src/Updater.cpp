#include "Updater.h"
#include <windows.h>
#include <winhttp.h>
#include <vector>
#include "Resources.h"
#include "Util.h"

namespace updater {
namespace {

constexpr const wchar_t* kLatestUrl = L"https://api.github.com/repos/sykrmzffr/Tandem/releases/latest";

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
};

std::wstring LastError(const wchar_t* what) {
    return std::wstring(what) + L" (error " + std::to_wstring(GetLastError()) + L")";
}

// GET `url` (HTTPS, redirects followed); calls `sink` with each chunk of the body.
template <typename Sink>
bool Get(const std::wstring& url, const wchar_t* accept, Sink sink, std::wstring& error) {
    URL_COMPONENTS uc{sizeof uc};
    wchar_t host[256] = {}, path[4096] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 4096;
    uc.dwExtraInfoLength = 1; // keep the query string as part of the path
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        error = L"bad address";
        return false;
    }
    std::wstring object = std::wstring(path, uc.dwUrlPathLength) +
                          (uc.lpszExtraInfo ? std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength) : L"");

    Handle session{WinHttpOpen(L"Tandem/" FD_VERSION_WSTR, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.h) {
        error = LastError(L"could not start HTTP");
        return false;
    }
    WinHttpSetTimeouts(session.h, 10000, 10000, 15000, 30000);
    Handle connect{WinHttpConnect(session.h, host, uc.nPort, 0)};
    Handle request{connect.h ? WinHttpOpenRequest(connect.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                  WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                  uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                             : nullptr};
    if (!request.h) {
        error = LastError(L"could not connect");
        return false;
    }
    std::wstring headers = std::wstring(L"Accept: ") + accept + L"\r\n";
    if (!WinHttpSendRequest(request.h, headers.c_str(), DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) {
        error = LastError(L"no answer from GitHub - check the internet connection");
        return false;
    }
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        error = status == 404 ? L"no release has been published yet" : L"GitHub answered " + std::to_wstring(status);
        return false;
    }
    std::vector<char> chunk(64 * 1024);
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(request.h, chunk.data(), DWORD(chunk.size()), &got)) {
            error = LastError(L"download interrupted");
            return false;
        }
        if (!got) return true;
        if (!sink(chunk.data(), got)) {
            error = L"could not save the download";
            return false;
        }
    }
}

// Value of the first "key":"value" string in `json` after `from`.
std::string JsonString(const std::string& json, const std::string& key, size_t from = 0, size_t* at = nullptr) {
    size_t k = json.find("\"" + key + "\"", from);
    if (k == std::string::npos) return {};
    size_t q = json.find('"', json.find(':', k) + 1);
    if (q == std::string::npos) return {};
    size_t e = json.find('"', q + 1);
    if (e == std::string::npos) return {};
    if (at) *at = e;
    return json.substr(q + 1, e - q - 1);
}

std::vector<int> Parts(std::wstring v) {
    if (!v.empty() && (v[0] == L'v' || v[0] == L'V')) v.erase(0, 1);
    std::vector<int> parts;
    for (size_t i = 0; i <= v.size();) {
        size_t dot = v.find(L'.', i);
        if (dot == std::wstring::npos) dot = v.size();
        parts.push_back(_wtoi(v.substr(i, dot - i).c_str()));
        i = dot + 1;
    }
    return parts;
}

} // namespace

bool Latest(Release& out, std::wstring& error) {
    std::string json;
    if (!Get(kLatestUrl, L"application/vnd.github+json",
             [&](const char* data, DWORD n) {
                 json.append(data, n);
                 return json.size() < 4 * 1024 * 1024;
             },
             error))
        return false;
    std::string tag = JsonString(json, "tag_name");
    if (tag.empty()) {
        error = L"unexpected answer from GitHub";
        return false;
    }
    out.version = Widen(tag[0] == 'v' ? tag.substr(1) : tag);
    out.pageUrl = Widen(JsonString(json, "html_url"));
    out.exeUrl.clear();
    for (size_t at = 0;;) {
        std::string url = JsonString(json, "browser_download_url", at, &at);
        if (url.empty()) break;
        if (url.size() > 11 && url.compare(url.size() - 11, 11, "/Tandem.exe") == 0) out.exeUrl = Widen(url);
    }
    return true;
}

bool IsNewer(const std::wstring& version) {
    std::vector<int> a = Parts(version), b = Parts(FD_VERSION_WSTR);
    for (size_t i = 0; i < a.size() || i < b.size(); i++) {
        int x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
        if (x != y) return x > y;
    }
    return false;
}

bool Download(const std::wstring& url, const std::wstring& path, std::wstring& error) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        error = LastError(L"could not create the download file");
        return false;
    }
    uint64_t total = 0;
    char head[2] = {};
    bool ok = Get(url, L"application/octet-stream",
                  [&](const char* data, DWORD n) {
                      if (total == 0 && n >= 2) memcpy(head, data, 2);
                      total += n;
                      DWORD wrote = 0;
                      return WriteFile(f, data, n, &wrote, nullptr) && wrote == n;
                  },
                  error);
    CloseHandle(f);
    if (ok && (head[0] != 'M' || head[1] != 'Z' || total < 1024 * 1024)) {
        error = L"the download is not a Windows program";
        ok = false;
    }
    if (!ok) DeleteFileW(path.c_str());
    else Log("Update: downloaded %llu bytes to %s", total, Narrow(path).c_str());
    return ok;
}

} // namespace updater
