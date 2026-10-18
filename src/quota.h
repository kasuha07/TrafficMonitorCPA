#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// 额度数据模型与解析。纯逻辑、不依赖网络，便于单元测试。
namespace cpa {

enum class Provider { Codex, Claude, Antigravity, Unknown };

struct QuotaWindow
{
    bool present = false;
    double used_percent = -1;   // 0~100；-1 表示未知
    int64_t reset_at = 0;       // Unix 秒；0 表示未知
    bool monthly = false;       // Codex 团队账号的次级窗口为月度

    // 剩余百分比；未知时返回 -1。
    double RemainingPercent() const;
};

struct Account
{
    std::string auth_index;
    std::string name;
    std::string email;
    std::string label;
    Provider provider = Provider::Unknown;
    bool disabled = false;
    std::string chatgpt_account_id;
    std::string project_id;     // Antigravity
    std::string plan;

    // 用于显示的账号名称：email > label > name。
    std::string DisplayName() const;
};

// Antigravity 按模型分组计算额度，每组各有 5h / 周窗口。
struct QuotaGroup
{
    std::string name;
    QuotaWindow five_hour;
    QuotaWindow weekly;
};

// 额度重置卡：Codex 的 rate-limit reset credits / Claude 的 reset grants（cedar_ember）。
struct ResetCredits
{
    int available = -1;          // 可用数量；-1 表示未知
    int64_t soonest_expiry = 0;  // 最早过期的可用卡的过期时间（Unix 秒）；0 表示未知
};

struct AccountQuota
{
    Account account;
    bool ok = false;
    std::wstring error;
    std::string plan;
    // Codex/Claude 直接填写；Antigravity 取自 Gemini 分组（用于任务栏汇总）。
    QuotaWindow five_hour;
    QuotaWindow weekly;
    std::vector<QuotaGroup> groups;   // 仅 Antigravity
    ResetCredits reset_credits;       // Codex / Claude
};

Provider ParseProvider(std::string_view raw);

// 解析 GET /v8/management/credentials 的响应，只保留支持的账号类型。
std::optional<std::vector<Account>> ParseCredentials(std::string_view body, std::string* error);

// 从凭证文件内容（credentials/download）中读取 project_id。
std::string ExtractProjectId(std::string_view credential_json);

// 构造 POST /v8/management/requests/api-call 的请求体。data 为空时不发送请求体。
std::string BuildApiCallBody(std::string_view auth_index, std::string_view method, std::string_view url,
                             const std::vector<std::pair<std::string, std::string>>& headers,
                             std::string_view data = {});

struct ApiCallResult
{
    unsigned status_code = 0;
    std::string body;
};

// 解析 api-call 的响应 {status_code, header, body}。
std::optional<ApiCallResult> ParseApiCallResponse(std::string_view body, std::string* error);

// 从上游/管理接口的错误响应体中提取可读信息。
std::string ExtractErrorMessage(std::string_view body);

// 解析 Codex wham/usage 响应。now 为当前 Unix 秒（用于 reset_after_seconds）。
bool ParseCodexUsage(std::string_view body, int64_t now, AccountQuota& out, std::string* error);

// 解析 Codex wham/rate-limit-reset-credits 响应（与 Management Center 的 resetCredits.ts 一致）。
bool ParseCodexResetCredits(std::string_view body, ResetCredits& out, std::string* error);

// 解析 Claude oauth/usage?cedar_ember=1 响应中的 cedar_ember 块：数量为各 grant 剩余次数之和。
// 与 Management Center 的 claudeResetGrants.ts 一致。
bool ParseClaudeResetGrants(std::string_view body, ResetCredits& out, std::string* error);

// 解析 Claude oauth/profile 响应中的套餐：Team / Max / Pro / Free；无法判断时返回空串。
// 与 Management Center 的 claude/data.ts resolveClaudePlanType 一致。
std::string ParseClaudePlan(std::string_view body);

// 解析 Claude oauth/usage 响应。
bool ParseClaudeUsage(std::string_view body, AccountQuota& out, std::string* error);

// 解析 Antigravity retrieveUserQuotaSummary 响应。
bool ParseAntigravityQuota(std::string_view body, AccountQuota& out, std::string* error);

// 解析 Antigravity loadCodeAssist 响应中的套餐（paidTier 优先于 currentTier），
// 返回 Free / Pro / Ultra / Ultra Lite，未知 tier 返回其名称或 ID；无法识别时返回空串。
std::string ParseAntigravityPlan(std::string_view body);

// ISO-8601（如 2026-10-18T12:00:00.123456+00:00 / Z）→ Unix 秒。
std::optional<int64_t> ParseIso8601(std::string_view s);

// 秒或毫秒级 Unix 时间戳 → Unix 秒。
std::optional<int64_t> NormalizeUnix(double v);

// 剩余时长的紧凑表示，如 "2h13m"、"3d4h"、"45m"。
std::wstring FormatDuration(int64_t seconds);

} // namespace cpa
