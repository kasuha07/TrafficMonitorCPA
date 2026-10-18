#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "quota.h"

namespace cpa {

// 以下提醒都在主线程中调用，返回的文本由调用方合并成一条通知。

// 低额度提醒：某个账号的某个窗口剩余跌破阈值时提醒一次；回升到阈值以上（通常是额度重置）后重新生效。
class LowQuotaAlerts
{
public:
    // 返回本次新触发的提醒文本（每条一行）。threshold 为 0 时关闭并清空状态。
    std::vector<std::wstring> Check(const std::vector<AccountQuota>& accounts, int threshold, int64_t now);

    void Reset() { alerted_.clear(); }

private:
    std::set<std::string> alerted_;
};

// 凭证失效提醒：账号凭证失效时提醒一次；该账号恢复正常或从账号列表中消失后重新生效。
class CredentialAlerts
{
public:
    // 返回本次新失效的账号（每条一行）。enabled 为 false 时关闭并清空状态。
    std::vector<std::wstring> Check(const std::vector<AccountQuota>& accounts, bool enabled);

    void Reset() { alerted_.clear(); }

private:
    std::set<std::string> alerted_;
};

// 额度恢复提醒：某个窗口用尽（剩余 0%）后再次获取到有余量的数据时提醒一次。
// 只看本次实际获取的数据，冷却中或沿用的旧数据不改变状态。
class RestoreAlerts
{
public:
    // 返回本次恢复的窗口（每条一行）。enabled 为 false 时关闭并清空状态。
    std::vector<std::wstring> Check(const std::vector<AccountQuota>& accounts, bool enabled, int64_t now);

    void Reset() { exhausted_.clear(); }

private:
    std::set<std::string> exhausted_;
};

} // namespace cpa
