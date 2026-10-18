#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "quota.h"

namespace cpa {

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

} // namespace cpa
