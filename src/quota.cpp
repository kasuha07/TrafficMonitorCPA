#include "quota.h"

#include <algorithm>
#include <cctype>
#include <ctime>

#include "json.h"

namespace cpa {

namespace {

constexpr double kFiveHourSeconds = 18000;
constexpr double kWeekSeconds = 604800;
constexpr double kMinMonthSeconds = 28.0 * 24 * 3600;
constexpr double kMaxMonthSeconds = 31.0 * 24 * 3600;

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
        std::string lower = *s;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
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
        auto inner = json::Parse(*root->AsString(), error);
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

        out.push_back(std::move(a));
    }
    std::stable_sort(out.begin(), out.end(), [](const Account& l, const Account& r) {
        if (l.provider != r.provider) return l.provider < r.provider;
        return l.DisplayName() < r.DisplayName();
    });
    return out;
}

std::string BuildApiCallBody(std::string_view auth_index, std::string_view method, std::string_view url,
                             const std::vector<std::pair<std::string, std::string>>& headers)
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
    body += "}}";
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
    if (const json::Value* b = root->Find("body")) {
        if (b->IsString()) {
            r.body = b->AsString().value_or("");
        }
    }
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
    if (!out.five_hour.present && !out.weekly.present) {
        if (error) *error = "usage response has no quota windows";
        return false;
    }
    return true;
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

} // namespace cpa
