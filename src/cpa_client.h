#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "quota.h"

namespace cpa {

struct Config
{
    std::wstring base_url = L"http://127.0.0.1:8317";
    std::wstring management_key;
    unsigned refresh_seconds = 300;

    // 未启用的账号类型不会请求上游，也不会出现在任务栏和鼠标提示中。
    bool enable_codex = true;
    bool enable_claude = true;
    bool enable_antigravity = true;

    AggregateMode aggregate = AggregateMode::Min;

    // 任务栏/鼠标提示显示已用百分比而不是剩余百分比。
    bool show_used = false;
    // 任务栏数值后附带重置倒计时，如 "82% 4h"。
    bool show_countdown = false;

    // 低额度提醒阈值（%）；0 表示关闭。
    unsigned alert_threshold = 10;

    bool IsEnabled(Provider p) const
    {
        switch (p) {
        case Provider::Codex: return enable_codex;
        case Provider::Claude: return enable_claude;
        case Provider::Antigravity: return enable_antigravity;
        default: return false;
        }
    }
};

struct Snapshot
{
    bool ok = false;
    // 管理密钥被拒绝（401/403）。CPA 连续 5 次认证失败会封禁 IP 30 分钟，
    // 因此出现该错误后应暂停自动刷新，直到用户修改设置。
    bool auth_rejected = false;
    std::wstring error;
    int64_t fetched_at = 0;
    std::vector<AccountQuota> accounts;
};

// 依次请求账号列表和每个账号的额度。同步阻塞，应在后台线程调用。
Snapshot FetchSnapshot(const Config& config);

// 只请求账号列表，用于选项对话框的"测试连接"。
std::wstring TestConnection(const Config& config);

} // namespace cpa
