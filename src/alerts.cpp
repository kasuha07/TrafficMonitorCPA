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

} // namespace

std::vector<std::wstring> LowQuotaAlerts::Check(const std::vector<AccountQuota>& accounts, int threshold, int64_t now)
{
    std::vector<std::wstring> messages;
    if (threshold <= 0) {
        alerted_.clear();
        return messages;
    }

    auto check = [&](const AccountQuota& q, const std::string& group, const QuotaWindow& w, bool five_hour) {
        double remaining = w.RemainingPercent();
        if (remaining < 0) return;
        const wchar_t* window = five_hour ? L"5h" : L"7d";
        std::string key = std::to_string(static_cast<int>(q.account.provider)) + "|" + q.account.auth_index + "|" +
                          q.account.name + "|" + group + "|" + (five_hour ? "5h" : "7d");
        if (remaining >= threshold) {
            alerted_.erase(key);
            return;
        }
        if (!alerted_.insert(key).second) return;

        std::wstring msg = std::wstring(ProviderName(q.account.provider)) + L" " + Widen(q.account.DisplayName());
        if (!group.empty()) msg += L" " + Widen(group);
        msg += L" " + std::wstring(w.monthly ? L"30d" : window) + T(L" 剩余 ", L" remaining ") +
               std::to_wstring(static_cast<int>(remaining + 0.5)) + L"%";
        if (w.reset_at > now) msg += L" (" + FormatDuration(w.reset_at - now) + L")";
        messages.push_back(std::move(msg));
    };

    for (const AccountQuota& q : accounts) {
        if (!q.ok) continue;  // 获取失败时保留原状态，避免恢复后重复提醒
        if (q.account.provider == Provider::Antigravity && !q.groups.empty()) {
            for (const QuotaGroup& g : q.groups) {
                check(q, g.name, g.five_hour, true);
                check(q, g.name, g.weekly, false);
            }
        } else {
            check(q, {}, q.five_hour, true);
            check(q, {}, q.weekly, false);
        }
    }
    return messages;
}

} // namespace cpa
