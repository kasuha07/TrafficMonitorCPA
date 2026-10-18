#include "cpa_client.h"

#include <ctime>

#include "http_client.h"

namespace cpa {

namespace {

// 与 Cli-Proxy-API-Management-Center (src/utils/quota/constants.ts) 保持一致。
constexpr char kCodexUsageUrl[] = "https://chatgpt.com/backend-api/wham/usage";
constexpr char kCodexUserAgent[] =
    "codex-tui/0.149.1 (Mac OS 26.5.2; arm64) iTerm.app/3.6.11 (codex-tui; 0.149.1)";
constexpr char kClaudeUsageUrl[] = "https://api.anthropic.com/api/oauth/usage";
constexpr char kClaudeUserAgent[] = "claude-cli/2.1.280 (external, cli)";

constexpr wchar_t kCredentialsPath[] = L"/v8/management/credentials";
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

void FetchAccountQuota(HttpClient& http, AccountQuota& q, int64_t now)
{
    const Account& a = q.account;
    std::vector<std::pair<std::string, std::string>> headers;
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

    HttpResponse r = http.PostJson(kApiCallPath, BuildApiCallBody(a.auth_index, "GET", url, headers));
    if (!r.ok || r.status != 200) {
        q.error = DescribeHttpFailure(r);
        return;
    }
    std::string err;
    auto call = ParseApiCallResponse(r.body, &err);
    if (!call) {
        q.error = L"解析 api-call 响应失败: " + Utf8ToWide(err);
        return;
    }
    if (call->status_code < 200 || call->status_code >= 300) {
        q.error = L"上游 HTTP " + std::to_wstring(call->status_code);
        std::string detail = ExtractErrorMessage(call->body);
        if (!detail.empty()) q.error += L": " + Utf8ToWide(detail);
        return;
    }

    bool parsed = a.provider == Provider::Codex ? ParseCodexUsage(call->body, now, q, &err)
                                                : ParseClaudeUsage(call->body, q, &err);
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

    size_t codex = 0, claude = 0;
    for (const Account& a : creds.accounts) {
        if (a.disabled) continue;
        if (a.provider == Provider::Codex) ++codex;
        else if (a.provider == Provider::Claude) ++claude;
    }
    return L"连接成功：Codex 账号 " + std::to_wstring(codex) + L" 个，Claude 账号 " +
           std::to_wstring(claude) + L" 个";
}

} // namespace cpa
