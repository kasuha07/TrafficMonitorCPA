#include "http_client.h"

#include <windows.h>
#include <winhttp.h>

namespace cpa {

std::wstring Utf8ToWide(std::string_view s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string WideToUtf8(std::wstring_view s)
{
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

namespace {

struct Handle
{
    HINTERNET h = nullptr;
    ~Handle() { if (h) WinHttpCloseHandle(h); }
};

std::wstring LastErrorText(const wchar_t* what)
{
    DWORD code = GetLastError();
    std::wstring msg = what;
    msg += L" failed (";
    msg += std::to_wstring(code);
    msg += L")";
    switch (code) {
    case ERROR_WINHTTP_CANNOT_CONNECT: msg += L": 无法连接到 CPA"; break;
    case ERROR_WINHTTP_TIMEOUT: msg += L": 请求超时"; break;
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: msg += L": 无法解析主机名"; break;
    case ERROR_WINHTTP_SECURE_FAILURE: msg += L": TLS 证书校验失败（自签名证书可在选项中跳过校验）"; break;
    default: break;
    }
    return msg;
}

} // namespace

HttpClient::HttpClient(std::wstring base_url, std::wstring bearer_token, unsigned timeout_ms, bool skip_tls_verify)
    : base_url_(std::move(base_url)), token_(std::move(bearer_token)), timeout_ms_(timeout_ms),
      skip_tls_verify_(skip_tls_verify)
{
    while (!base_url_.empty() && base_url_.back() == L'/') base_url_.pop_back();
    session_ = WinHttpOpen(L"CPAQuota", WINHTTP_ACCESS_TYPE_NO_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
}

HttpClient::~HttpClient()
{
    if (session_) WinHttpCloseHandle(static_cast<HINTERNET>(session_));
}

HttpResponse HttpClient::Get(std::wstring_view path)
{
    return Send(L"GET", path, {});
}

HttpResponse HttpClient::PostJson(std::wstring_view path, std::string_view json_body)
{
    return Send(L"POST", path, json_body);
}

HttpResponse HttpClient::Send(const wchar_t* method, std::wstring_view path, std::string_view body)
{
    HttpResponse resp;
    if (!session_) {
        resp.error = L"WinHttpOpen failed";
        return resp;
    }

    std::wstring url = base_url_ + std::wstring(path);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256]{};
    wchar_t url_path[2048]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = static_cast<DWORD>(std::size(host));
    uc.lpszUrlPath = url_path;
    uc.dwUrlPathLength = static_cast<DWORD>(std::size(url_path));
    wchar_t extra[2048]{};
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        resp.error = L"CPA 地址无效: " + base_url_;
        return resp;
    }
    bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
    std::wstring object = std::wstring(url_path) + extra;

    auto* session = static_cast<HINTERNET>(session_);
    WinHttpSetTimeouts(session, static_cast<int>(timeout_ms_), static_cast<int>(timeout_ms_),
                       static_cast<int>(timeout_ms_), static_cast<int>(timeout_ms_));

    Handle connect{WinHttpConnect(session, host, uc.nPort, 0)};
    if (!connect.h) {
        resp.error = LastErrorText(L"WinHttpConnect");
        return resp;
    }
    Handle request{WinHttpOpenRequest(connect.h, method, object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0)};
    if (!request.h) {
        resp.error = LastErrorText(L"WinHttpOpenRequest");
        return resp;
    }
    if (secure && skip_tls_verify_) {
        DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        if (!WinHttpSetOption(request.h, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags))) {
            resp.error = LastErrorText(L"WinHttpSetOption(SECURITY_FLAGS)");
            return resp;
        }
    }

    std::wstring headers = L"Accept: application/json\r\n";
    if (!token_.empty()) headers += L"Authorization: Bearer " + token_ + L"\r\n";
    if (!body.empty()) headers += L"Content-Type: application/json\r\n";

    BOOL sent = WinHttpSendRequest(request.h, headers.c_str(), static_cast<DWORD>(headers.size()),
                                   body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                                   static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    if (!sent) {
        resp.error = LastErrorText(L"WinHttpSendRequest");
        return resp;
    }
    if (!WinHttpReceiveResponse(request.h, nullptr)) {
        resp.error = LastErrorText(L"WinHttpReceiveResponse");
        return resp;
    }

    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    resp.status = status;

    constexpr size_t kMaxBody = 16u * 1024 * 1024;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available)) {
            resp.error = LastErrorText(L"WinHttpQueryDataAvailable");
            return resp;
        }
        if (available == 0) break;
        if (resp.body.size() + available > kMaxBody) {
            resp.error = L"响应过大";
            return resp;
        }
        size_t old = resp.body.size();
        resp.body.resize(old + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.h, resp.body.data() + old, available, &read)) {
            resp.error = LastErrorText(L"WinHttpReadData");
            return resp;
        }
        resp.body.resize(old + read);
    }
    resp.ok = true;
    return resp;
}

} // namespace cpa
