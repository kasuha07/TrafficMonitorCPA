#include "cpa_client.h"

#include <cctype>
#include <ctime>
#include <map>
#include <mutex>

#include "http_client.h"
#include "json.h"

namespace cpa {

namespace {

// 与 Cli-Proxy-API-Management-Center (src/utils/quota/constants.ts) 保持一致。
constexpr char kCodexUsageUrl[] = "https://chatgpt.com/backend-api/wham/usage";
constexpr char kCodexUserAgent[] =
    "codex-tui/0.149.1 (Mac OS 26.5.2; arm64) iTerm.app/3.6.11 (codex-tui; 0.149.1)";
constexpr char kClaudeUsageUrl[] = "https://api.anthropic.com/api/oauth/usage";
constexpr char kClaudeUserAgent[] = "claude-cli/2.1.280 (external, cli)";
constexpr const char* kAntigravityQuotaUrls[] = {
    "https://daily-cloudcode-pa.googleapis.com/v1internal:retrieveUserQuotaSummary",
    "https://daily-cloudcode-pa.sandbox.googleapis.com/v1internal:retrieveUserQuotaSummary",
    "https://cloudcode-pa.googleapis.com/v1internal:retrieveUserQuotaSummary",
};
constexpr char kAntigravityUserAgent[] = "antigravity/hub/2.23.0 darwin/arm64";

constexpr wchar_t kCredentialsPath[] = L"/v8/management/credentials";
constexpr wchar_t kDownloadPath[] = L"/v8/management/credentials/download?name=";
constexpr wchar_t kApiCallPath[] = L"/v8/management/requests/api-call";

std::wstring DescribeHttpFailure(const HttpResponse& r)
{
    if (!r.ok) return r.error;
    std::wstring msg = L"HTTP " + std::to_wstring(r.status);
    std::string detail = ExtractErrorMessage(r.body);
    if (!detail.empty()) msg += L": " + Utf8ToWide(detail);
    return msg;
}

bool IsAuthRejected(const HttpResponse& r)
{
    return r.ok && (r.status == 401 || r.status == 403);
}

struct CredentialsResult
{
    bool ok = false;
    bool auth_rejected = false;
    std::wstring error;
    std::vector<Account> accounts;
};

CredentialsResult FetchCredentials(HttpClient& http)
{
    CredentialsResult result;
    HttpResponse r = http.Get(kCredentialsPath);
    if (!r.ok || r.status != 200) {
        result.auth_rejected = IsAuthRejected(r);
        result.error = L"获取账号列表失败: " + DescribeHttpFailure(r);
        if (r.ok && r.status == 404) result.error += L"（CPA 版本是否支持 /v8/management？）";
        return result;
    }
    std::string err;
    auto accounts = ParseCredentials(r.body, &err);
    if (!accounts) {
        result.error = L"解析账号列表失败: " + Utf8ToWide(err);
        return result;
    }
    result.ok = true;
    result.accounts = std::move(*accounts);
    return result;
}

using Headers = std::vector<std::pair<std::string, std::string>>;

// 通过 api-call 让 CPA 用账号 token 请求上游。成功（上游 2xx）时返回 true 并写入 body。
bool CallUpstream(HttpClient& http, const Account& a, const char* method, const char* url,
                  const Headers& headers, std::string_view data, std::string& body, std::wstring& error)
{
    HttpResponse r = http.PostJson(kApiCallPath, BuildApiCallBody(a.auth_index, method, url, headers, data));
    if (!r.ok || r.status != 200) {
        error = DescribeHttpFailure(r);
        return false;
    }
    std::string err;
    auto call = ParseApiCallResponse(r.body, &err);
    if (!call) {
        error = L"解析 api-call 响应失败: " + Utf8ToWide(err);
        return false;
    }
    if (call->status_code < 200 || call->status_code >= 300) {
        error = L"上游 HTTP " + std::to_wstring(call->status_code);
        std::string detail = ExtractErrorMessage(call->body);
        if (!detail.empty()) error += L": " + Utf8ToWide(detail);
        return false;
    }
    body = std::move(call->body);
    return true;
}

std::wstring UrlEncode(std::string_view s)
{
    static const char hex[] = "0123456789ABCDEF";
    std::wstring out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<wchar_t>(c);
        } else {
            out += L'%';
            out += static_cast<wchar_t>(hex[c >> 4]);
            out += static_cast<wchar_t>(hex[c & 0xF]);
        }
    }
    return out;
}

// 账号列表里没有 project_id 时，读取凭证文件获取（与 Management Center 一致）。结果按账号缓存。
std::string ResolveProjectId(HttpClient& http, const Account& a)
{
    if (!a.project_id.empty()) return a.project_id;

    static std::mutex cache_mutex;
    static std::map<std::string, std::string> cache;
    std::string key = a.auth_index + "|" + a.name;
    {
        std::lock_guard lock(cache_mutex);
        if (auto it = cache.find(key); it != cache.end()) return it->second;
    }
    if (a.name.empty()) return {};
    HttpResponse r = http.Get(std::wstring(kDownloadPath) + UrlEncode(a.name));
    if (!r.ok || r.status != 200) return {};
    std::string id = ExtractProjectId(r.body);
    if (!id.empty()) {
        std::lock_guard lock(cache_mutex);
        cache[key] = id;
    }
    return id;
}

void FetchAntigravityQuota(HttpClient& http, AccountQuota& q)
{
    std::string project = ResolveProjectId(http, q.account);
    if (project.empty()) {
        q.error = L"缺少 project_id";
        return;
    }
    const Headers headers = {
        {"Authorization", "Bearer $TOKEN$"},
        {"Content-Type", "application/json"},
        {"User-Agent", kAntigravityUserAgent},
    };
    std::string data = "{\"project\":" + json::Quote(project) + "}";

    // 依次尝试多个端点，直到拿到可解析的额度。
    std::wstring last_error;
    for (const char* url : kAntigravityQuotaUrls) {
        std::string body;
        if (!CallUpstream(http, q.account, "POST", url, headers, data, body, last_error)) continue;
        std::string err;
        if (ParseAntigravityQuota(body, q, &err)) {
            q.ok = true;
            return;
        }
        last_error = L"解析额度失败: " + Utf8ToWide(err);
    }
    q.error = last_error;
}

void FetchAccountQuota(HttpClient& http, AccountQuota& q, int64_t now)
{
    const Account& a = q.account;
    if (a.provider == Provider::Antigravity) {
        FetchAntigravityQuota(http, q);
        return;
    }

    Headers headers;
    const char* url = nullptr;
    if (a.provider == Provider::Codex) {
        url = kCodexUsageUrl;
        headers = {
            {"Authorization", "Bearer $TOKEN$"},
            {"Content-Type", "application/json"},
            {"User-Agent", kCodexUserAgent},
        };
        if (!a.chatgpt_account_id.empty()) headers.emplace_back("Chatgpt-Account-Id", a.chatgpt_account_id);
    } else if (a.provider == Provider::Claude) {
        url = kClaudeUsageUrl;
        headers = {
            {"User-Agent", kClaudeUserAgent},
            {"Authorization", "Bearer $TOKEN$"},
            {"Content-Type", "application/json"},
            {"anthropic-beta", "oauth-2025-04-20"},
            {"Accept", "application/json"},
        };
    } else {
        q.error = L"不支持的账号类型";
        return;
    }

    std::string body;
    if (!CallUpstream(http, a, "GET", url, headers, {}, body, q.error)) return;

    std::string err;
    bool parsed = a.provider == Provider::Codex ? ParseCodexUsage(body, now, q, &err)
                                                : ParseClaudeUsage(body, q, &err);
    if (!parsed) {
        q.error = L"解析额度失败: " + Utf8ToWide(err);
        return;
    }
    if (q.plan.empty()) q.plan = a.plan;
    q.ok = true;
}

} // namespace

Snapshot FetchSnapshot(const Config& config)
{
    Snapshot snap;
    snap.fetched_at = static_cast<int64_t>(std::time(nullptr));
    if (config.management_key.empty()) {
        snap.error = L"未设置 CPA 管理密钥，请在插件选项中配置";
        return snap;
    }

    HttpClient http(config.base_url, config.management_key);
    CredentialsResult creds = FetchCredentials(http);
    if (!creds.ok) {
        snap.auth_rejected = creds.auth_rejected;
        snap.error = creds.error;
        return snap;
    }

    for (Account& a : creds.accounts) {
        if (a.disabled) continue;
        AccountQuota q;
        q.account = std::move(a);
        FetchAccountQuota(http, q, static_cast<int64_t>(std::time(nullptr)));
        snap.accounts.push_back(std::move(q));
    }
    snap.ok = true;
    return snap;
}

std::wstring TestConnection(const Config& config)
{
    if (config.management_key.empty()) return L"请填写管理密钥";
    HttpClient http(config.base_url, config.management_key, 10000);
    CredentialsResult creds = FetchCredentials(http);
    if (!creds.ok) return creds.error;

    size_t codex = 0, claude = 0, antigravity = 0;
    for (const Account& a : creds.accounts) {
        if (a.disabled) continue;
        if (a.provider == Provider::Codex) ++codex;
        else if (a.provider == Provider::Claude) ++claude;
        else if (a.provider == Provider::Antigravity) ++antigravity;
    }
    return L"连接成功：Codex " + std::to_wstring(codex) + L" 个，Claude " + std::to_wstring(claude) +
           L" 个，Antigravity " + std::to_wstring(antigravity) + L" 个账号";
}

} // namespace cpa
