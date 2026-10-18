#pragma once

#include <string>
#include <string_view>

namespace cpa {

struct HttpResponse
{
    bool ok = false;          // 是否收到了 HTTP 响应（与状态码无关）
    unsigned status = 0;
    std::string body;         // 原始响应体（UTF-8）
    std::wstring error;       // 传输层错误
};

// 连接 CPA 时使用的代理。数值与 ini 中保存的值一致。
enum class ProxyMode { None = 0, System = 1, Custom = 2 };

// 校验并规范化自定义代理地址（host:port 或 http://host:port，WinHTTP 只支持 HTTP 代理）。
// 返回 false 表示格式不支持。
bool NormalizeProxy(std::wstring_view input, std::wstring& out);

// 基于 WinHTTP 的同步客户端，面向单个 CPA 实例。Get/PostJson 可在多个线程中同时调用。
class HttpClient
{
public:
    // skip_tls_verify 为 true 时忽略 HTTPS 证书错误（自签名、过期、主机名不匹配），仅用于自建 CPA。
    // proxy_mode 为 Custom 时 proxy 为 NormalizeProxy 规范化后的地址。
    HttpClient(std::wstring base_url, std::wstring bearer_token, unsigned timeout_ms = 20000,
               bool skip_tls_verify = false, ProxyMode proxy_mode = ProxyMode::None, std::wstring_view proxy = {});
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // path 以 '/' 开头，相对于 base_url。
    HttpResponse Get(std::wstring_view path);
    HttpResponse PostJson(std::wstring_view path, std::string_view json_body);

private:
    HttpResponse Send(const wchar_t* method, std::wstring_view path, std::string_view body);

    std::wstring base_url_;
    std::wstring token_;
    unsigned timeout_ms_;
    bool skip_tls_verify_;
    void* session_ = nullptr;
};

std::wstring Utf8ToWide(std::string_view s);
std::string WideToUtf8(std::wstring_view s);

} // namespace cpa
