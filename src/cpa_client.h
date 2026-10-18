#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "quota.h"

namespace cpa {

// 双击任务栏显示项的动作。数值与 ini 中保存的值一致。
enum class DoubleClickAction { OpenManagementPage = 0, Refresh = 1 };

struct Config
{
    std::wstring base_url = L"http://127.0.0.1:8317";
    std::wstring management_key;
    unsigned refresh_seconds = 300;

    // 未启用的账号类型不会请求上游，也不会出现在任务栏和鼠标提示中。默认全部关闭，需在选项中勾选。
    bool enable_codex = false;
    bool enable_claude = false;
    bool enable_antigravity = false;

    AggregateMode aggregate = AggregateMode::Min;

    // 任务栏/鼠标提示显示已用百分比而不是剩余百分比。
    bool show_used = false;
    // 任务栏数值后附带重置倒计时，如 "82% 4h"。
    bool show_countdown = false;
    // 按剩余比例给数值着色（插件自绘，此时 TrafficMonitor 中自定义的标签文字不生效）。
    bool color_by_quota = false;

    DoubleClickAction double_click = DoubleClickAction::OpenManagementPage;

    // 智能冷却：额度用尽的账号在重置前不再请求，沿用上次数据。
    bool smart_cooldown = true;

    // 跳过 CPA 的 HTTPS 证书校验（自签名证书等）。只影响插件到 CPA 的连接。
    bool skip_tls_verify = false;

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
    int64_t fetched_at = 0;   // 数据获取时间；沿用旧数据时为旧数据的时间
    std::vector<AccountQuota> accounts;

    // 获取账号列表失败但沿用了上次数据时：连续失败次数与失败原因。
    int failures = 0;
    std::wstring warning;
};

// 依次请求账号列表和每个账号的额度。同步阻塞，应在后台线程调用。
// previous 为上一次的结果，用于智能冷却；force 为 true（手动刷新）时忽略冷却。
Snapshot FetchSnapshot(const Config& config, const Snapshot* previous = nullptr, bool force = false);

// 只请求账号列表，用于选项对话框的"测试连接"。
std::wstring TestConnection(const Config& config);

} // namespace cpa
