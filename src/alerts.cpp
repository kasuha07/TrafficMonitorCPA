#include "alerts.h"
#include "i18n.h"

#include <windows.h>

namespace cpa {

namespace {

std::wstring Widen(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

const wchar_t* ProviderName(Provider p)
{
    switch (p) {
    case Provider::Codex: return L"Codex";
    case Provider::Claude: return L"Claude";
    case Provider::Antigravity: return L"Antigravity";
    default: return L"?";
    }
}

std::string AccountKey(const Account& a)
{
    return std::to_string(static_cast<int>(a.provider)) + "|" + a.auth_index + "|" + a.name;
}

std::wstring AccountLabel(const Account& a)
{
    return std::wstring(ProviderName(a.provider)) + L" " + Widen(a.DisplayName());
}

// 对账号的每个窗口调用 fn(group, window, five_hour)；Antigravity 按模型分组展开。
template <typename Fn>
void ForEachWindow(const AccountQuota& q, Fn&& fn)
{
    if (q.account.provider == Provider::Antigravity && !q.groups.empty()) {
        for (const QuotaGroup& g : q.groups) {
            fn(g.name, g.five_hour, true);
            fn(g.name, g.weekly, false);
        }
    } else {
        fn(std::string(), q.five_hour, true);
        fn(std::string(), q.weekly, false);
    }
}

std::string WindowKey(const AccountQuota& q, const std::string& group, bool five_hour)
{
    return AccountKey(q.account) + "|" + group + "|" + (five_hour ? "5h" : "7d");
}

// 「Codex user@example.com Gemini 5h 剩余 8% (2h13m)」
std::wstring DescribeWindow(const AccountQuota& q, const std::string& group, const QuotaWindow& w, bool five_hour,
                            double remaining, int64_t now)
{
    std::wstring msg = AccountLabel(q.account);
    if (!group.empty()) msg += L" " + Widen(group);
    msg += L" " + std::wstring(w.monthly ? L"30d" : (five_hour ? L"5h" : L"7d")) + T(L" 剩余 ", L" remaining ") +
           std::to_wstring(static_cast<int>(remaining + 0.5)) + L"%";
    if (w.reset_at > now) msg += L" (" + FormatDuration(w.reset_at - now) + L")";
    return msg;
}

} // namespace

std::vector<std::wstring> LowQuotaAlerts::Check(const std::vector<AccountQuota>& accounts, int threshold, int64_t now)
{
    std::vector<std::wstring> messages;
    if (threshold <= 0) {
        alerted_.clear();
        return messages;
    }

    for (const AccountQuota& q : accounts) {
        if (!q.ok) continue;  // 获取失败时保留原状态，避免恢复后重复提醒
        ForEachWindow(q, [&](const std::string& group, const QuotaWindow& w, bool five_hour) {
            double remaining = w.RemainingPercent();
            if (remaining < 0) return;
            std::string key = WindowKey(q, group, five_hour);
            if (remaining >= threshold) {
                alerted_.erase(key);
                return;
            }
            if (alerted_.insert(key).second)
                messages.push_back(DescribeWindow(q, group, w, five_hour, remaining, now));
        });
    }
    return messages;
}

std::vector<std::wstring> CredentialAlerts::Check(const std::vector<AccountQuota>& accounts, bool enabled)
{
    std::vector<std::wstring> messages;
    if (!enabled) {
        alerted_.clear();
        return messages;
    }

    std::set<std::string> present;
    for (const AccountQuota& q : accounts) {
        std::string key = AccountKey(q.account);
        present.insert(key);
        if (q.credential_invalid) {
            if (alerted_.insert(key).second) messages.push_back(AccountLabel(q.account));
        } else if (q.ok && !q.stale && !q.cooled) {
            alerted_.erase(key);  // 再次获取成功，视为已重新登录
        }
    }
    std::erase_if(alerted_, [&](const std::string& key) { return !present.count(key); });
    return messages;
}

std::vector<std::wstring> RestoreAlerts::Check(const std::vector<AccountQuota>& accounts, bool enabled, int64_t now)
{
    std::vector<std::wstring> messages;
    if (!enabled) {
        exhausted_.clear();
        return messages;
    }

    for (const AccountQuota& q : accounts) {
        if (!q.ok || q.stale || q.cooled) continue;
        ForEachWindow(q, [&](const std::string& group, const QuotaWindow& w, bool five_hour) {
            double remaining = w.RemainingPercent();
            if (remaining < 0) return;
            std::string key = WindowKey(q, group, five_hour);
            if (remaining < 0.5) {
                exhausted_.insert(key);
            } else if (exhausted_.erase(key)) {
                messages.push_back(DescribeWindow(q, group, w, five_hour, remaining, now));
            }
        });
    }
    return messages;
}

} // namespace cpa
