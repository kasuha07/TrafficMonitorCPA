#include "cpa_client.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "http_client.h"
#include "i18n.h"
#include "json.h"

namespace cpa {

namespace {

// 与 Cli-Proxy-API-Management-Center (src/utils/quota/constants.ts) 保持一致。
constexpr char kCodexUsageUrl[] = "https://chatgpt.com/backend-api/wham/usage";
constexpr char kCodexResetCreditsUrl[] = "https://chatgpt.com/backend-api/wham/rate-limit-reset-credits";
constexpr char kCodexUserAgent[] =
    "codex-tui/0.149.1 (Mac OS 26.5.2; arm64) iTerm.app/3.6.11 (codex-tui; 0.149.1)";
constexpr char kClaudeUsageUrl[] = "https://api.anthropic.com/api/oauth/usage";
constexpr char kClaudeUserAgent[] = "claude-cli/2.1.280 (external, cli)";
constexpr char kClaudeProfileUrl[] = "https://api.anthropic.com/api/oauth/profile";
constexpr char kClaudeResetGrantsUrl[] = "https://api.anthropic.com/api/oauth/usage?cedar_ember=1&skip_spend=1";
constexpr const char* kAntigravityQuotaUrls[] = {
    "https://daily-cloudcode-pa.googleapis.com/v1internal:retrieveUserQuotaSummary",
    "https://daily-cloudcode-pa.sandbox.googleapis.com/v1internal:retrieveUserQuotaSummary",
    "https://cloudcode-pa.googleapis.com/v1internal:retrieveUserQuotaSummary",
};
constexpr char kAntigravityCodeAssistUrl[] = "https://daily-cloudcode-pa.googleapis.com/v1internal:loadCodeAssist";
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
        result.error = T(L"获取账号列表失败: ", L"Failed to fetch accounts: ") + DescribeHttpFailure(r);
        if (r.ok && r.status == 404)
            result.error += T(L"（CPA 版本是否支持 /v8/management？）", L" (does this CPA version support /v8/management?)");
        return result;
    }
    std::string err;
    auto accounts = ParseCredentials(r.body, &err);
    if (!accounts) {
        result.error = T(L"解析账号列表失败: ", L"Failed to parse accounts: ") + Utf8ToWide(err);
        return result;
    }
    result.ok = true;
    result.accounts = std::move(*accounts);
    return result;
}

using Headers = std::vector<std::pair<std::string, std::string>>;

// 通过 api-call 让 CPA 用账号 token 请求上游。成功（上游 2xx）时返回 true 并写入 body。
// 失败且原因是凭证失效时，若传入 credential_invalid 则将其置为 true。
bool CallUpstream(HttpClient& http, const Account& a, const char* method, const char* url,
                  const Headers& headers, std::string_view data, std::string& body, std::wstring& error,
                  bool* credential_invalid = nullptr)
{
    HttpResponse r = http.PostJson(kApiCallPath, BuildApiCallBody(a.auth_index, method, url, headers, data));
    if (!r.ok || r.status != 200) {
        error = DescribeHttpFailure(r);
        if (credential_invalid && r.ok && IsCredentialFailure(r.status, r.body, 0)) *credential_invalid = true;
        return false;
    }
    std::string err;
    auto call = ParseApiCallResponse(r.body, &err);
    if (!call) {
        error = T(L"解析 api-call 响应失败: ", L"Failed to parse api-call response: ") + Utf8ToWide(err);
        return false;
    }
    if (call->status_code < 200 || call->status_code >= 300) {
        error = T(L"上游 HTTP ", L"Upstream HTTP ") + std::to_wstring(call->status_code);
        std::string detail = ExtractErrorMessage(call->body);
        if (!detail.empty()) error += L": " + Utf8ToWide(detail);
        if (credential_invalid && IsCredentialFailure(r.status, r.body, call->status_code)) *credential_invalid = true;
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

// 按账号缓存很少变化的附加信息（套餐、重置卡等），减少对上游的请求。每条记录有各自的有效期。
template <typename T>
class TtlCache
{
public:
    std::optional<T> Get(const Account& a, int64_t now)
    {
        std::lock_guard lock(mutex_);
        auto it = entries_.find(Key(a));
        if (it == entries_.end() || now >= it->second.first) return std::nullopt;
        return it->second.second;
    }

    void Put(const Account& a, int64_t now, int64_t ttl_seconds, T value)
    {
        std::lock_guard lock(mutex_);
        entries_[Key(a)] = {now + ttl_seconds, std::move(value)};
    }

private:
    static std::string Key(const Account& a) { return a.auth_index + "|" + a.name; }

    std::mutex mutex_;
    std::map<std::string, std::pair<int64_t, T>> entries_;  // 键 → (过期时间, 值)
};

// 可选信息查询失败（或结果无法识别）后，在这段时间内不再重试，避免每轮刷新都请求一次。
constexpr int64_t kRetryAfterFailureSeconds = 600;
// 套餐、重置卡等很少变化的信息，成功后半天内不再请求。
constexpr int64_t kStableInfoSeconds = 12 * 3600;
// 成功结果「永久」有效（直到插件重新加载）。
constexpr int64_t kForeverSeconds = int64_t{1} << 40;

int64_t Now()
{
    return static_cast<int64_t>(std::time(nullptr));
}

// 账号列表里没有 project_id 时，读取凭证文件获取（与 Management Center 一致）。结果按账号缓存。
std::string ResolveProjectId(HttpClient& http, const Account& a)
{
    if (!a.project_id.empty()) return a.project_id;
    if (a.name.empty()) return {};

    static TtlCache<std::string> cache;
    const int64_t now = Now();
    if (auto cached = cache.Get(a, now)) return *cached;

    HttpResponse r = http.Get(std::wstring(kDownloadPath) + UrlEncode(a.name));
    std::string id = r.ok && r.status == 200 ? ExtractProjectId(r.body) : std::string();
    cache.Put(a, now, id.empty() ? kRetryAfterFailureSeconds : kForeverSeconds, id);
    return id;
}

// 套餐几乎不变，每个账号最多半天查询一次；失败时不显示套餐，不影响额度。
std::string FetchAntigravityPlan(HttpClient& http, const Account& a, const Headers& headers)
{
    static TtlCache<std::string> cache;
    const int64_t now = Now();
    if (auto cached = cache.Get(a, now)) return *cached;

    std::string body;
    std::wstring error;
    std::string plan;
    if (CallUpstream(http, a, "POST", kAntigravityCodeAssistUrl, headers,
                     R"({"metadata":{"ideType":"ANTIGRAVITY"}})", body, error))
        plan = ParseAntigravityPlan(body);
    cache.Put(a, now, plan.empty() ? kRetryAfterFailureSeconds : kStableInfoSeconds, plan);
    return plan;
}

// 同上，Claude 套餐来自 oauth/profile。
std::string FetchClaudePlan(HttpClient& http, const Account& a, const Headers& headers)
{
    static TtlCache<std::string> cache;
    const int64_t now = Now();
    if (auto cached = cache.Get(a, now)) return *cached;

    std::string body;
    std::wstring error;
    std::string plan;
    if (CallUpstream(http, a, "GET", kClaudeProfileUrl, headers, {}, body, error)) plan = ParseClaudePlan(body);
    cache.Put(a, now, plan.empty() ? kRetryAfterFailureSeconds : kStableInfoSeconds, plan);
    return plan;
}

void FetchAntigravityQuota(HttpClient& http, AccountQuota& q)
{
    std::string project = ResolveProjectId(http, q.account);
    if (project.empty()) {
        q.error = T(L"缺少 project_id", L"Missing project_id");
        return;
    }
    const Headers headers = {
        {"Authorization", "Bearer $TOKEN$"},
        {"Content-Type", "application/json"},
        {"User-Agent", kAntigravityUserAgent},
    };
    std::string data = "{\"project\":" + json::Quote(project) + "}";

    // 依次尝试多个端点，直到拿到可解析的额度。上次成功的端点先试，不必每轮都先请求不可用的端点。
    static TtlCache<size_t> working_endpoint;
    constexpr size_t kEndpointCount = std::size(kAntigravityQuotaUrls);
    const size_t first = working_endpoint.Get(q.account, Now()).value_or(0);
    std::wstring last_error;
    for (size_t n = 0; n < kEndpointCount; ++n) {
        const size_t index = (first + n) % kEndpointCount;
        std::string body;
        if (!CallUpstream(http, q.account, "POST", kAntigravityQuotaUrls[index], headers, data, body, last_error,
                          &q.credential_invalid))
            continue;
        std::string err;
        if (ParseAntigravityQuota(body, q, &err)) {
            q.ok = true;
            q.credential_invalid = false;
            if (index != first) working_endpoint.Put(q.account, Now(), kForeverSeconds, index);
            q.plan = FetchAntigravityPlan(http, q.account, headers);
            return;
        }
        last_error = T(L"解析额度失败: ", L"Failed to parse quota: ") + Utf8ToWide(err);
    }
    q.error = last_error;
}

// 重置卡明细是可选信息：失败时保留用量响应里的数量，不影响额度显示。
// 明细很少变化，每个账号最多半天请求一次；期间数量仍以每轮用量响应里的为准，只沿用过期时间。
void FetchCodexResetCredits(HttpClient& http, AccountQuota& q, Headers headers, int64_t now)
{
    static TtlCache<ResetCredits> cache;
    if (auto cached = cache.Get(q.account, now)) {
        if (q.reset_credits.available < 0) q.reset_credits.available = cached->available;
        q.reset_credits.soonest_expiry = cached->soonest_expiry;
        return;
    }

    headers.emplace_back("Accept", "application/json");
    headers.emplace_back("OpenAI-Beta", "codex-1");
    headers.emplace_back("Originator", "Codex Desktop");
    std::string body;
    std::wstring error;
    ResetCredits detail;
    std::string err;
    if (CallUpstream(http, q.account, "GET", kCodexResetCreditsUrl, headers, {}, body, error) &&
        ParseCodexResetCredits(body, detail, &err)) {
        if (detail.available >= 0) q.reset_credits.available = detail.available;
        q.reset_credits.soonest_expiry = detail.soonest_expiry;
        cache.Put(q.account, now, kStableInfoSeconds, detail);
    } else {
        cache.Put(q.account, now, kRetryAfterFailureSeconds, ResetCredits{});  // 失败时缓存「未知」
    }
}

// Claude 重置卡复用用量接口（带 cedar_ember 参数），该接口容易被限流，
// 且 grant 很少变化，所以每个账号最多半天请求一次，期间使用缓存（用掉重置卡后数量最多滞后半天）。
void FetchClaudeResetGrants(HttpClient& http, AccountQuota& q, const Headers& headers, int64_t now)
{
    static TtlCache<ResetCredits> cache;
    if (auto cached = cache.Get(q.account, now)) {
        q.reset_credits = *cached;
        return;
    }

    std::string body;
    std::wstring error;
    if (!CallUpstream(http, q.account, "GET", kClaudeResetGrantsUrl, headers, {}, body, error)) return;
    ResetCredits credits;
    std::string err;
    if (!ParseClaudeResetGrants(body, credits, &err)) return;
    q.reset_credits = credits;
    cache.Put(q.account, now, kStableInfoSeconds, credits);
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
        q.error = T(L"不支持的账号类型", L"Unsupported account type");
        return;
    }

    std::string body;
    if (!CallUpstream(http, a, "GET", url, headers, {}, body, q.error, &q.credential_invalid)) return;

    std::string err;
    bool parsed = a.provider == Provider::Codex ? ParseCodexUsage(body, now, q, &err)
                                                : ParseClaudeUsage(body, q, &err);
    if (!parsed) {
        q.error = T(L"解析额度失败: ", L"Failed to parse quota: ") + Utf8ToWide(err);
        return;
    }
    if (q.plan.empty()) q.plan = a.plan;
    q.ok = true;

    if (a.provider == Provider::Codex) {
        FetchCodexResetCredits(http, q, headers, now);
    } else {
        FetchClaudeResetGrants(http, q, headers, now);
        if (q.plan.empty()) q.plan = FetchClaudePlan(http, a, headers);
    }
}

} // namespace

namespace {

// 多次刷新之间复用同一个 WinHTTP 会话：保留空闲连接和代理自动发现的结果，
// 不必每轮都重新做 TCP/TLS 握手。连接相关的设置改变时重新创建。
std::shared_ptr<HttpClient> SharedClient(const Config& config)
{
    struct Entry
    {
        std::wstring base_url, key, proxy;
        bool skip_tls_verify = false;
        ProxyMode proxy_mode = ProxyMode::None;
        std::shared_ptr<HttpClient> client;
    };
    static std::mutex mutex;
    // 有意不释放：避免在 DLL 卸载（持有加载器锁）时关闭 WinHTTP 句柄。
    static Entry* entry = new Entry();

    std::lock_guard lock(mutex);
    if (!entry->client || entry->base_url != config.base_url || entry->key != config.management_key ||
        entry->proxy != config.proxy || entry->skip_tls_verify != config.skip_tls_verify ||
        entry->proxy_mode != config.proxy_mode) {
        entry->base_url = config.base_url;
        entry->key = config.management_key;
        entry->proxy = config.proxy;
        entry->skip_tls_verify = config.skip_tls_verify;
        entry->proxy_mode = config.proxy_mode;
        // 旧会话由仍在使用它的请求持有，用完后自动关闭。
        entry->client = std::make_shared<HttpClient>(config.base_url, config.management_key, 20000,
                                                     config.skip_tls_verify, config.proxy_mode, config.proxy);
    }
    return entry->client;
}

const AccountQuota* FindPrevious(const Snapshot* previous, const Account& a)
{
    if (!previous || !previous->ok) return nullptr;
    for (const AccountQuota& q : previous->accounts) {
        if (q.account.provider == a.provider && q.account.auth_index == a.auth_index && q.account.name == a.name)
            return &q;
    }
    return nullptr;
}

} // namespace

Snapshot FetchSnapshot(const Config& config, const Snapshot* previous, bool force)
{
    Snapshot snap;
    snap.fetched_at = static_cast<int64_t>(std::time(nullptr));
    if (config.management_key.empty()) {
        snap.error = T(L"未设置 CPA 管理密钥，请在插件选项中配置", L"CPA management key is not set. Configure it in plugin options.");
        return snap;
    }
    if (!config.enable_codex && !config.enable_claude && !config.enable_antigravity) {
        snap.error = T(L"未勾选任何账号类型，请在插件选项的「查询账号」中勾选", L"No account types selected. Select them under Query in plugin options.");
        return snap;
    }

    const std::shared_ptr<HttpClient> client = SharedClient(config);
    HttpClient& http = *client;
    CredentialsResult creds = FetchCredentials(http);
    if (!creds.ok) {
        // 网络抖动等临时故障：沿用上次数据，连续失败达到上限后再显示错误。密钥错误不沿用。
        const int failures = (previous ? previous->failures : 0) + 1;
        if (!creds.auth_rejected && previous && previous->ok && failures < kMaxCarriedFailures) {
            Snapshot carried = *previous;
            carried.failures = failures;
            carried.warning = creds.error;
            for (AccountQuota& q : carried.accounts) q.cooled = false;
            return carried;
        }
        snap.auth_rejected = creds.auth_rejected;
        snap.error = creds.error;
        return snap;
    }

    // 先确定哪些账号需要请求（冷却中的直接沿用），保持账号列表的顺序。
    std::vector<size_t> pending;
    const int64_t start = static_cast<int64_t>(std::time(nullptr));
    for (Account& a : creds.accounts) {
        if (a.disabled || !config.IsEnabled(a.provider)) continue;

        if (config.smart_cooldown && !force) {
            const AccountQuota* prev = FindPrevious(previous, a);
            if (prev && CooldownUntil(*prev, start) > start) {
                AccountQuota q = *prev;
                q.account = std::move(a);  // 账号元数据（邮箱、备注等）用最新的
                ProjectResets(q, start);
                q.cooled = true;
                q.stale = false;
                snap.accounts.push_back(std::move(q));
                continue;
            }
        }

        AccountQuota q;
        q.account = std::move(a);
        pending.push_back(snap.accounts.size());
        snap.accounts.push_back(std::move(q));
    }

    // 最多 kMaxConcurrency 个账号同时请求，避免一个慢上游拖慢整轮刷新。
    constexpr size_t kMaxConcurrency = 4;
    std::atomic<size_t> next{0};
    auto worker = [&]() {
        for (size_t i = next++; i < pending.size(); i = next++) {
            AccountQuota& q = snap.accounts[pending[i]];
            const int64_t now = static_cast<int64_t>(std::time(nullptr));
            FetchAccountQuota(http, q, now);
            if (q.ok) q.data_at = now;
            else CarryOverOnFailure(FindPrevious(previous, q.account), q);
        }
    };
    std::vector<std::thread> threads;
    const size_t thread_count = std::min(kMaxConcurrency, pending.size());
    for (size_t i = 1; i < thread_count; ++i) threads.emplace_back(worker);
    if (thread_count > 0) worker();  // 当前线程也参与
    for (std::thread& t : threads) t.join();

    snap.ok = true;
    return snap;
}

std::wstring TestConnection(const Config& config)
{
    if (config.management_key.empty()) return T(L"请填写管理密钥", L"Please enter the management key");
    HttpClient http(config.base_url, config.management_key, 10000, config.skip_tls_verify, config.proxy_mode,
                    config.proxy);
    CredentialsResult creds = FetchCredentials(http);
    if (!creds.ok) return creds.error;

    size_t codex = 0, claude = 0, antigravity = 0;
    for (const Account& a : creds.accounts) {
        if (a.disabled) continue;
        if (a.provider == Provider::Codex) ++codex;
        else if (a.provider == Provider::Claude) ++claude;
        else if (a.provider == Provider::Antigravity) ++antigravity;
    }
    if (IsEnglish()) {
        return L"Connected: Codex " + std::to_wstring(codex) + L", Claude " + std::to_wstring(claude) +
               L", Antigravity " + std::to_wstring(antigravity) + L" accounts";
    }
    return L"连接成功：Codex " + std::to_wstring(codex) + L" 个，Claude " + std::to_wstring(claude) +
           L" 个，Antigravity " + std::to_wstring(antigravity) + L" 个账号";
}

} // namespace cpa
