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
    unsigned refresh_seconds = 60;
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
