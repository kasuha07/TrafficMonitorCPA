#include "quota.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>

#include "json.h"

namespace cpa {

namespace {

constexpr double kFiveHourSeconds = 18000;
constexpr double kWeekSeconds = 604800;
constexpr double kMinMonthSeconds = 28.0 * 24 * 3600;
constexpr double kMaxMonthSeconds = 31.0 * 24 * 3600;

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string StringOf(const json::Value* v)
{
    if (!v) return {};
    if (auto s = v->AsString()) return *s;
    return {};
}

std::optional<double> NumberOf(const json::Value* v)
{
    if (!v) return std::nullopt;
    return v->AsNumber();
}

bool TruthyOf(const json::Value* v)
{
    if (!v) return false;
    if (auto b = v->AsBool()) return *b;
    if (auto n = v->AsNumber()) return *n != 0;
    if (auto s = v->AsString()) {
        std::string lower = Lower(*s);
        return lower == "true" || lower == "1" || lower == "yes";
    }
    return false;
}

// API 响应体可能是 JSON 字符串里再套 JSON；这里统一解析成对象。
std::optional<json::Value> ParseObject(std::string_view body, std::string* error)
{
    auto root = json::Parse(body, error);
    if (!root) return std::nullopt;
    if (root->IsString()) {
        auto inner = json::Parse(root->StringView(), error);
        if (!inner) return std::nullopt;
        root = std::move(inner);
    }
    if (!root->IsObject()) {
        if (error) *error = "response is not a JSON object";
        return std::nullopt;
    }
    return root;
}

std::optional<double> WindowSeconds(const json::Value* w)
{
    return w ? NumberOf(w->FindAny({"limit_window_seconds", "limitWindowSeconds"})) : std::nullopt;
}

bool IsMonthly(const json::Value* w)
{
    auto s = WindowSeconds(w);
    return s && *s >= kMinMonthSeconds && *s <= kMaxMonthSeconds;
}

QuotaWindow BuildCodexWindow(const json::Value* w, bool limit_reached, int64_t now)
{
    QuotaWindow out;
    if (!w || !w->IsObject()) return out;
    out.present = true;
    out.monthly = IsMonthly(w);

    if (auto reset = NumberOf(w->FindAny({"reset_at", "resetAt"}))) {
        if (auto unix = NormalizeUnix(*reset)) out.reset_at = *unix;
    } else if (auto iso = StringOf(w->FindAny({"reset_at", "resetAt"})); !iso.empty()) {
        if (auto t = ParseIso8601(iso)) out.reset_at = *t;
    }
    if (out.reset_at == 0) {
        if (auto after = NumberOf(w->FindAny({"reset_after_seconds", "resetAfterSeconds"})); after && *after > 0)
            out.reset_at = now + static_cast<int64_t>(*after);
    }

    if (auto used = NumberOf(w->FindAny({"used_percent", "usedPercent"}))) {
        out.used_percent = *used;
    } else if (limit_reached && out.reset_at != 0) {
        out.used_percent = 100;
    }
    return out;
}

} // namespace

double QuotaWindow::RemainingPercent() const
{
    if (!present || used_percent < 0) return -1;
    return std::clamp(100.0 - used_percent, 0.0, 100.0);
}

AggregateResult Aggregate(const std::vector<double>& remaining_percents, AggregateMode mode)
{
    AggregateResult r;
    double sum = 0;
    for (size_t i = 0; i < remaining_percents.size(); ++i) {
        double p = remaining_percents[i];
        if (p < 0) continue;
        ++r.total;
        if (p >= 0.5) ++r.available;
        sum += p;
        if (r.percent < 0 || (mode == AggregateMode::Min && p < r.percent) ||
            (mode == AggregateMode::Max && p > r.percent)) {
            r.percent = p;
            r.index = static_cast<int>(i);
        }
    }
    if (mode != AggregateMode::Min && mode != AggregateMode::Max) r.index = -1;
    if (r.total > 0 && mode == AggregateMode::Average) r.percent = sum / r.total;
    return r;
}

namespace {

// 窗口组在何时之前不可用；组内没有用尽的窗口时返回 0。
int64_t GroupBlockedUntil(const QuotaWindow& five_hour, const QuotaWindow& weekly, int64_t now)
{
    int64_t until = 0;
    for (const QuotaWindow* w : {&five_hour, &weekly}) {
        double remaining = w->RemainingPercent();
        if (remaining < 0 || remaining >= 0.5 || w->reset_at <= now) continue;
        until = std::max(until, w->reset_at);
    }
    return until;
}

} // namespace

int64_t CooldownUntil(const AccountQuota& q, int64_t now)
{
    if (!q.ok) return 0;
    if (q.account.provider == Provider::Antigravity && !q.groups.empty()) {
        int64_t earliest = 0;
        for (const QuotaGroup& g : q.groups) {
            int64_t until = GroupBlockedUntil(g.five_hour, g.weekly, now);
            if (until == 0) return 0;  // 还有可用的分组，需要继续刷新
            if (earliest == 0 || until < earliest) earliest = until;
        }
        return earliest;
    }
    return GroupBlockedUntil(q.five_hour, q.weekly, now);
}

void ProjectResets(AccountQuota& q, int64_t now)
{
    auto project = [now](QuotaWindow& w) {
        if (w.present && w.reset_at > 0 && w.reset_at <= now) {
            w.used_percent = 0;
            w.reset_at = 0;
        }
    };
    project(q.five_hour);
    project(q.weekly);
    for (QuotaGroup& g : q.groups) {
        project(g.five_hour);
        project(g.weekly);
    }
}

bool CarryOverOnFailure(const AccountQuota* previous, AccountQuota& failed)
{
    const int failures = (previous ? previous->failures : 0) + 1;
    if (!previous || !previous->ok || failures >= kMaxCarriedFailures) {
        failed.failures = failures;
        return false;
    }
    AccountQuota carried = *previous;
    carried.account = std::move(failed.account);
    carried.error = std::move(failed.error);
    carried.credential_invalid = failed.credential_invalid;
    carried.failures = failures;
    carried.stale = true;
    carried.cooled = false;
    failed = std::move(carried);
    return true;
}

std::string Account::DisplayName() const
{
    if (!email.empty()) return email;
    if (!label.empty()) return label;
    return name;
}

Provider ParseProvider(std::string_view raw)
{
    std::string key(raw);
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    key.erase(0, key.find_first_not_of(" \t"));
    key.erase(key.find_last_not_of(" \t") + 1);
    if (key == "codex") return Provider::Codex;
    if (key == "claude") return Provider::Claude;
    if (key == "antigravity") return Provider::Antigravity;
    return Provider::Unknown;
}

std::optional<std::vector<Account>> ParseCredentials(std::string_view body, std::string* error)
{
    auto root = json::Parse(body, error);
    if (!root) return std::nullopt;
    const json::Value* files = root->IsObject() ? root->Find("files") : nullptr;
    if (!files || !files->IsArray()) {
        if (error) *error = "credentials response has no 'files' array";
        return std::nullopt;
    }

    std::vector<Account> out;
    for (const auto& f : files->Items()) {
        if (!f.IsObject()) continue;
        Account a;
        std::string provider = StringOf(f.FindAny({"provider", "type"}));
        a.provider = ParseProvider(provider);
        if (a.provider == Provider::Unknown) continue;

        if (auto idx = f.Find("auth_index")) {
            if (auto s = idx->AsString()) a.auth_index = *s;
            else if (auto n = idx->AsNumber()) a.auth_index = std::to_string(static_cast<long long>(*n));
        }
        if (a.auth_index.empty()) continue;

        a.name = StringOf(f.Find("name"));
        a.email = StringOf(f.Find("email"));
        a.label = StringOf(f.Find("label"));
        a.disabled = TruthyOf(f.Find("disabled"));

        if (const json::Value* id_token = f.Find("id_token"); id_token && id_token->IsObject()) {
            a.chatgpt_account_id = StringOf(id_token->FindAny({"chatgpt_account_id", "chatgptAccountId"}));
            a.plan = StringOf(id_token->FindAny({"plan_type", "planType"}));
        }
        if (a.chatgpt_account_id.empty())
            a.chatgpt_account_id = StringOf(f.FindAny({"chatgpt_account_id", "chatgptAccountId"}));
        a.project_id = StringOf(f.FindAny({"project_id", "projectId"}));

        out.push_back(std::move(a));
    }
    std::stable_sort(out.begin(), out.end(), [](const Account& l, const Account& r) {
        if (l.provider != r.provider) return l.provider < r.provider;
        return l.DisplayName() < r.DisplayName();
    });
    return out;
}

std::string ExtractProjectId(std::string_view credential_json)
{
    std::string err;
    auto root = json::Parse(credential_json, &err);
    if (!root || !root->IsObject()) return {};
    if (auto id = StringOf(root->FindAny({"project_id", "projectId"})); !id.empty()) return id;
    for (const char* section : {"installed", "web"}) {
        const json::Value* v = root->Find(section);
        if (v && v->IsObject()) {
            if (auto id = StringOf(v->FindAny({"project_id", "projectId"})); !id.empty()) return id;
        }
    }
    return {};
}

std::string BuildApiCallBody(std::string_view auth_index, std::string_view method, std::string_view url,
                             const std::vector<std::pair<std::string, std::string>>& headers,
                             std::string_view data)
{
    std::string body = "{\"auth_index\":" + json::Quote(auth_index) +
                       ",\"method\":" + json::Quote(method) +
                       ",\"url\":" + json::Quote(url) + ",\"header\":{";
    bool first = true;
    for (const auto& [k, v] : headers) {
        if (!first) body += ',';
        first = false;
        body += json::Quote(k) + ':' + json::Quote(v);
    }
    body += '}';
    if (!data.empty()) body += ",\"data\":" + json::Quote(data);
    body += '}';
    return body;
}

std::optional<ApiCallResult> ParseApiCallResponse(std::string_view body, std::string* error)
{
    auto root = json::Parse(body, error);
    if (!root) return std::nullopt;
    if (!root->IsObject()) {
        if (error) *error = "api-call response is not an object";
        return std::nullopt;
    }
    auto status = NumberOf(root->FindAny({"status_code", "statusCode"}));
    if (!status) {
        if (error) *error = "api-call response has no status_code";
        return std::nullopt;
    }
    ApiCallResult r;
    r.status_code = static_cast<unsigned>(*status);
    // 上游响应体可能较大，直接取走解析出的字符串，不再复制。
    if (json::Value* b = root->Find("body")) r.body = b->TakeString();
    return r;
}

std::string ExtractErrorMessage(std::string_view body)
{
    std::string err;
    auto root = json::Parse(body, &err);
    if (root && root->IsObject()) {
        if (const json::Value* e = root->Find("error")) {
            if (e->IsObject()) {
                if (auto m = StringOf(e->Find("message")); !m.empty()) return m;
            } else if (auto s = e->AsString()) {
                return *s;
            }
        }
        if (auto m = StringOf(root->Find("message")); !m.empty()) return m;
    }
    std::string text(body.substr(0, 200));
    text.erase(std::remove_if(text.begin(), text.end(), [](char c) { return c == '\r' || c == '\n'; }), text.end());
    return text;
}

bool IsCredentialFailure(unsigned cpa_status, std::string_view cpa_body, unsigned upstream_status)
{
    if (cpa_status == 200) return upstream_status == 401;
    if (cpa_status != 400) return false;
    // 与 CPA management/api_tools.go 中 $TOKEN$ 解析失败时的错误文本一致。
    std::string message = ExtractErrorMessage(cpa_body);
    return message == "auth token refresh failed" || message == "auth token not found";
}

bool ParseCodexUsage(std::string_view body, int64_t now, AccountQuota& out, std::string* error)
{
    auto root = ParseObject(body, error);
    if (!root) return false;

    if (auto plan = StringOf(root->FindAny({"plan_type", "planType"})); !plan.empty()) out.plan = plan;

    const json::Value* rate = root->FindAny({"rate_limit", "rateLimit"});
    if (!rate || !rate->IsObject()) {
        if (error) *error = "usage response has no rate_limit";
        return false;
    }
    bool limit_reached = TruthyOf(rate->FindAny({"limit_reached", "limitReached"}));
    if (const json::Value* allowed = rate->Find("allowed"); allowed && allowed->AsBool() == false)
        limit_reached = true;

    const json::Value* primary = rate->FindAny({"primary_window", "primaryWindow"});
    const json::Value* secondary = rate->FindAny({"secondary_window", "secondaryWindow"});

    // 优先按窗口时长归类，旧格式无时长时按 primary/secondary 顺序兜底（与 Management Center 一致）。
    const json::Value* five = nullptr;
    const json::Value* week = nullptr;
    for (const json::Value* w : {primary, secondary}) {
        if (!w || !w->IsObject()) continue;
        auto s = WindowSeconds(w);
        if (s && *s == kFiveHourSeconds && !five) five = w;
        else if (s && (*s == kWeekSeconds || IsMonthly(w)) && !week) week = w;
    }
    if (!five && primary && primary->IsObject() && primary != week) five = primary;
    if (!week && secondary && secondary->IsObject() && secondary != five) week = secondary;

    out.five_hour = BuildCodexWindow(five, limit_reached, now);
    out.weekly = BuildCodexWindow(week, limit_reached, now);

    // 用量响应里也带有重置卡数量，作为单独接口失败时的兜底。
    if (const json::Value* credits = root->FindAny({"rate_limit_reset_credits", "rateLimitResetCredits"});
        credits && credits->IsObject()) {
        if (auto n = NumberOf(credits->FindAny({"available_count", "availableCount"})))
            out.reset_credits.available = static_cast<int>(*n);
    }
    if (!out.five_hour.present && !out.weekly.present) {
        if (error) *error = "usage response has no quota windows";
        return false;
    }
    return true;
}

bool ParseCodexResetCredits(std::string_view body, ResetCredits& out, std::string* error)
{
    auto root = ParseObject(body, error);
    if (!root) return false;
    const json::Value* count = root->FindAny({"available_count", "availableCount"});
    const json::Value* credits = root->Find("credits");
    if (!count && !credits) {
        if (error) *error = "reset credits response has unexpected shape";
        return false;
    }

    // 只统计 codex_rate_limits 类型、状态为 available 且带过期时间的卡。
    int listed = 0;
    int64_t soonest = 0;
    if (credits && credits->IsArray()) {
        for (const json::Value& c : credits->Items()) {
            if (!c.IsObject()) continue;
            if (StringOf(c.FindAny({"reset_type", "resetType"})) != "codex_rate_limits") continue;
            if (StringOf(c.Find("status")) != "available") continue;
            std::string expires = StringOf(c.FindAny({"expires_at", "expiresAt"}));
            if (expires.empty()) continue;
            ++listed;
            auto t = ParseIso8601(expires);
            if (!t) {
                if (auto n = NumberOf(c.FindAny({"expires_at", "expiresAt"}))) t = NormalizeUnix(*n);
            }
            if (t && (soonest == 0 || *t < soonest)) soonest = *t;
        }
    }

    ResetCredits result;
    if (auto n = NumberOf(count)) result.available = static_cast<int>(*n);
    else if (listed > 0) result.available = listed;
    result.soonest_expiry = soonest;
    out = result;
    return true;
}

bool ParseClaudeResetGrants(std::string_view body, ResetCredits& out, std::string* error)
{
    auto root = ParseObject(body, error);
    if (!root) return false;
    const json::Value* block = root->Find("cedar_ember");
    if (!block || !block->IsObject() || !block->Find("eligible") || !block->Find("eligible")->IsBool()) {
        if (error) *error = "usage response has no cedar_ember block";
        return false;
    }

    ResetCredits result;
    result.available = 0;
    if (const json::Value* grants = block->Find("grants"); grants && grants->IsArray()) {
        for (const json::Value& g : grants->Items()) {
            if (!g.IsObject()) continue;
            auto left = NumberOf(g.Find("resets_left"));
            if (!left || *left <= 0) continue;
            result.available += static_cast<int>(*left);
            if (auto ends = ParseIso8601(StringOf(g.Find("ends_at")))) {
                if (result.soonest_expiry == 0 || *ends < result.soonest_expiry) result.soonest_expiry = *ends;
            }
        }
    }
    out = result;
    return true;
}

std::string ParseClaudePlan(std::string_view body)
{
    std::string err;
    auto root = ParseObject(body, &err);
    if (!root) return {};

    if (const json::Value* org = root->Find("organization"); org && org->IsObject()) {
        if (Lower(StringOf(org->Find("organization_type"))) == "claude_team" &&
            Lower(StringOf(org->Find("subscription_status"))) == "active")
            return "Team";
    }

    // 标记可能是布尔、数字或 "true"/"yes" 等字符串；缺失时为未知。
    auto flag = [](const json::Value* v) -> std::optional<bool> {
        if (!v || v->IsNull()) return std::nullopt;
        if (auto b = v->AsBool()) return *b;
        if (auto n = v->AsNumber()) return *n != 0;
        if (auto s = v->AsString()) {
            std::string l = Lower(*s);
            if (l == "true" || l == "1" || l == "yes" || l == "y" || l == "on") return true;
            if (l == "false" || l == "0" || l == "no" || l == "n" || l == "off") return false;
        }
        return std::nullopt;
    };
    const json::Value* account = root->Find("account");
    if (!account || !account->IsObject()) return {};
    auto max = flag(account->Find("has_claude_max"));
    if (max == true) return "Max";
    auto pro = flag(account->Find("has_claude_pro"));
    if (pro == true) return "Pro";
    if (max == false && pro == false) return "Free";
    return {};
}

bool ParseClaudeUsage(std::string_view body, AccountQuota& out, std::string* error)
{
    auto root = ParseObject(body, error);
    if (!root) return false;

    auto read = [&](const char* key) {
        QuotaWindow w;
        const json::Value* v = root->Find(key);
        if (!v || !v->IsObject()) return w;
        auto util = NumberOf(v->Find("utilization"));
        if (!util) return w;
        w.present = true;
        w.used_percent = *util;
        const json::Value* reset = v->Find("resets_at");
        if (reset) {
            if (auto s = reset->AsString()) {
                if (auto t = ParseIso8601(*s)) w.reset_at = *t;
            } else if (auto n = reset->AsNumber()) {
                if (auto t = NormalizeUnix(*n)) w.reset_at = *t;
            }
        }
        return w;
    };

    out.five_hour = read("five_hour");
    out.weekly = read("seven_day");
    if (!out.five_hour.present && !out.weekly.present) {
        if (error) *error = "usage response has no five_hour/seven_day windows";
        return false;
    }
    return true;
}

namespace {

enum class BucketKind { FiveHour, Weekly, Other };

// 与 Management Center 的 antigravity/labels.ts 一致：先按 window 字段，再按显示文案。
BucketKind ClassifyBucket(const std::string& window, const std::string& label)
{
    std::string w = Lower(window);
    if (w == "5h" || w == "five-hour" || w == "five_hour") return BucketKind::FiveHour;
    if (w == "weekly" || w == "week") return BucketKind::Weekly;
    std::string l = Lower(label);
    if (l.find("weekly") != std::string::npos) return BucketKind::Weekly;
    if (l.find("5 hour") != std::string::npos || l.find("5-hour") != std::string::npos ||
        l.find("five hour") != std::string::npos || l.find("five-hour") != std::string::npos)
        return BucketKind::FiveHour;
    return BucketKind::Other;
}

// remainingFraction 可能是 0~1 的数字、数字字符串或 "85%"。
std::optional<double> FractionOf(const json::Value* v)
{
    if (!v) return std::nullopt;
    if (auto n = v->AsNumber()) return *n;
    if (auto s = v->AsString(); s && !s->empty() && s->back() == '%') {
        char* end = nullptr;
        std::string num = s->substr(0, s->size() - 1);
        double d = std::strtod(num.c_str(), &end);
        if (end == num.c_str() + num.size()) return d / 100;
    }
    return std::nullopt;
}

std::string ShortGroupName(const std::string& name)
{
    std::string l = Lower(name);
    if (l == "gemini models") return "Gemini";
    if (l == "claude and gpt models") return "Claude/GPT";
    return name;
}

} // namespace

bool ParseAntigravityQuota(std::string_view body, AccountQuota& out, std::string* error)
{
    auto root = ParseObject(body, error);
    if (!root) return false;
    const json::Value* groups = root->Find("groups");
    if (!groups || !groups->IsArray()) {
        if (error) *error = "quota summary has no groups";
        return false;
    }

    out.groups.clear();
    int index = 0;
    for (const json::Value& g : groups->Items()) {
        ++index;
        if (!g.IsObject()) continue;
        QuotaGroup group;
        std::string raw_name = StringOf(g.FindAny({"displayName", "display_name"}));
        group.name = raw_name.empty() ? "Group " + std::to_string(index) : ShortGroupName(raw_name);

        const json::Value* buckets = g.Find("buckets");
        if (!buckets || !buckets->IsArray()) continue;
        for (const json::Value& b : buckets->Items()) {
            if (!b.IsObject()) continue;
            auto fraction = FractionOf(b.FindAny({"remainingFraction", "remaining_fraction"}));
            if (!fraction) continue;
            std::string label = StringOf(b.FindAny({"displayName", "display_name"}));
            if (label.empty()) label = StringOf(b.Find("description"));
            BucketKind kind = ClassifyBucket(StringOf(b.Find("window")), label);
            if (kind == BucketKind::Other) continue;

            QuotaWindow w;
            w.present = true;
            w.used_percent = 100.0 - *fraction * 100.0;
            if (auto reset = StringOf(b.FindAny({"resetTime", "reset_time"})); !reset.empty()) {
                if (auto t = ParseIso8601(reset)) w.reset_at = *t;
            }
            (kind == BucketKind::FiveHour ? group.five_hour : group.weekly) = w;
        }
        if (group.five_hour.present || group.weekly.present) out.groups.push_back(std::move(group));
    }

    if (out.groups.empty()) {
        if (error) *error = "quota summary has no 5h/weekly buckets";
        return false;
    }

    // 任务栏汇总取 Gemini 分组；没有时取第一个分组。
    const QuotaGroup* primary = &out.groups.front();
    for (const QuotaGroup& g : out.groups) {
        if (g.name == "Gemini") {
            primary = &g;
            break;
        }
    }
    out.five_hour = primary->five_hour;
    out.weekly = primary->weekly;
    return true;
}

std::string ParseAntigravityPlan(std::string_view body)
{
    std::string err;
    auto root = ParseObject(body, &err);
    if (!root) return {};
    auto tier = [&](std::initializer_list<std::string_view> keys) -> std::pair<std::string, std::string> {
        const json::Value* t = root->FindAny(keys);
        if (!t || !t->IsObject()) return {};
        return {StringOf(t->Find("id")), StringOf(t->Find("name"))};
    };
    auto paid = tier({"paidTier", "paid_tier"});
    auto effective = paid.first.empty() ? tier({"currentTier", "current_tier"}) : paid;

    // 与 Management Center 的 antigravitySubscription.ts 一致。
    const std::string& id = effective.first;
    if (id == "free-tier") return "Free";
    if (id == "g1-pro-tier") return "Pro";
    if (id == "g1-ultra-tier") return "Ultra";
    if (id == "g1-ultra-lite-tier") return "Ultra Lite";
    return !effective.second.empty() ? effective.second : id;
}

std::optional<int64_t> ParseIso8601(std::string_view s)
{
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    size_t i = 0;
    auto digits = [&](int count, int& v) {
        v = 0;
        for (int k = 0; k < count; ++k, ++i) {
            if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
            v = v * 10 + (s[i] - '0');
        }
        return true;
    };
    auto expect = [&](char c) {
        if (i < s.size() && s[i] == c) { ++i; return true; }
        return false;
    };
    while (i < s.size() && s[i] == ' ') ++i;
    if (!digits(4, y) || !expect('-') || !digits(2, mo) || !expect('-') || !digits(2, d)) return std::nullopt;
    if (i < s.size() && (s[i] == 'T' || s[i] == 't' || s[i] == ' ')) {
        ++i;
        if (!digits(2, h) || !expect(':') || !digits(2, mi)) return std::nullopt;
        if (expect(':') && !digits(2, sec)) return std::nullopt;
        if (expect('.')) {
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        }
    }
    int offset = 0;
    if (i < s.size()) {
        if (s[i] == 'Z' || s[i] == 'z') {
            ++i;
        } else if (s[i] == '+' || s[i] == '-') {
            int sign = s[i] == '-' ? -1 : 1;
            ++i;
            int oh = 0, om = 0;
            if (!digits(2, oh)) return std::nullopt;
            expect(':');
            if (i < s.size()) digits(2, om);
            offset = sign * (oh * 3600 + om * 60);
        }
    }
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 60) return std::nullopt;

    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = sec;
    int64_t t = _mkgmtime64(&tm);
    if (t == -1) return std::nullopt;
    return t - offset;
}

std::optional<int64_t> NormalizeUnix(double v)
{
    if (!(v > 0)) return std::nullopt;
    return static_cast<int64_t>(v < 1e11 ? v : v / 1000);
}

std::wstring FormatDuration(int64_t seconds)
{
    if (seconds <= 0) return L"0m";
    int64_t minutes = (seconds + 59) / 60;
    int64_t days = minutes / (24 * 60);
    int64_t hours = (minutes / 60) % 24;
    int64_t mins = minutes % 60;
    if (days > 0) return std::to_wstring(days) + L"d" + std::to_wstring(hours) + L"h";
    if (hours > 0) return std::to_wstring(hours) + L"h" + std::to_wstring(mins) + L"m";
    return std::to_wstring(mins) + L"m";
}

std::wstring FormatDurationShort(int64_t seconds)
{
    if (seconds <= 0) return L"0m";
    int64_t minutes = (seconds + 59) / 60;
    if (minutes < 60) return std::to_wstring(minutes) + L"m";
    int64_t hours = minutes / 60;
    if (hours < 24) return std::to_wstring(hours) + L"h";
    return std::to_wstring(hours / 24) + L"d";
}

} // namespace cpa
