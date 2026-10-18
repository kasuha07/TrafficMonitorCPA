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
    bool cooled = false;              // 本次因冷却未请求，沿用上次数据
    bool stale = false;               // 本次请求失败，沿用上次成功的数据（error 为失败原因）
    int failures = 0;                 // 连续失败次数
    int64_t data_at = 0;              // 数据获取时间（Unix 秒）
};

// 连续失败达到该次数时不再沿用旧数据，直接显示错误。
constexpr int kMaxCarriedFailures = 3;

// 请求失败时：若上次有成功数据且连续失败次数未达上限，则沿用上次数据（标记为 stale）并返回 true；
// 否则只累计失败次数，返回 false。failed 为本次失败的结果（含最新账号信息和 error）。
bool CarryOverOnFailure(const AccountQuota* previous, AccountQuota& failed);

// 智能冷却：账号额度用尽时，到重置前都不必再请求。
// 每个「窗口组」（Codex/Claude 为 5h+7d 一组，Antigravity 为每个模型分组）若有用尽且已知重置时间的窗口，
// 则该组在其中最晚的重置时间前不可用；所有组都不可用时，账号冷却到最早恢复的那一组。
// 返回冷却截止时间（Unix 秒）；不需要冷却时返回 0。
int64_t CooldownUntil(const AccountQuota& q, int64_t now);

// 沿用旧数据时，把已过重置时间的窗口视为已重置（剩余 100%，下次重置时间未知）。
void ProjectResets(AccountQuota& q, int64_t now);

Provider ParseProvider(std::string_view raw);

// 任务栏显示项如何汇总同类多个账号的剩余百分比。数值与 ini 中保存的值一致，不要调整顺序。
enum class AggregateMode { Min = 0, Average = 1, Max = 2, AvailableCount = 3 };

struct AggregateResult
{
    int total = 0;          // 参与汇总的账号数（剩余未知的不计入）
    int available = 0;      // 剩余未用尽（四舍五入后 > 0%）的账号数
    double percent = -1;    // Min / Average / Max 的结果；没有数据时为 -1
    int index = -1;         // Min / Max 选中的输入下标；其他模式为 -1
};

AggregateResult Aggregate(const std::vector<double>& remaining_percents, AggregateMode mode);

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

// 任务栏用的更短表示，只保留最大单位（小时、天向下取整），如 "4h"、"3d"、"45m"。
std::wstring FormatDurationShort(int64_t seconds);

} // namespace cpa
