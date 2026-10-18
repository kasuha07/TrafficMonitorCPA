#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "PluginInterface.h"
#include "alerts.h"
#include "cpa_client.h"

namespace cpa {

enum class WindowKind { FiveHour, Weekly };

// 一个显示项目 = 一种账号类型的一个额度窗口，显示所有该类型账号中最低的剩余百分比。
class QuotaItem final : public IPluginItem
{
public:
    QuotaItem(Provider provider, WindowKind window, const wchar_t* name, const wchar_t* id, const wchar_t* label);

    const wchar_t* GetItemName() const override { return name_; }
    const wchar_t* GetItemId() const override { return id_; }
    const wchar_t* GetItemLableText() const override { return label_; }
    const wchar_t* GetItemValueText() const override { return value_.c_str(); }
    const wchar_t* GetItemValueSampleText() const override { return sample_; }
    int IsDrawResourceUsageGraph() const override { return 1; }
    int OnMouseEvent(MouseEventType type, int x, int y, void* hWnd, int flag) override;
    float GetResourceUsageGraphValue() const override;

    // 仅在主线程（DataRequired）中调用。
    void Update(const Snapshot* snapshot, const Config& config);

private:
    Provider provider_;
    WindowKind window_;
    const wchar_t* name_;
    const wchar_t* id_;
    const wchar_t* label_;
    std::wstring value_{L"--"};
    const wchar_t* sample_ = L"100%";  // 决定显示宽度，随倒计时设置变化
    double graph_ = -1;     // 资源占用图的值（0~1）；-1 表示无数据
};

class CpaPlugin final : public ITMPlugin
{
public:
    static CpaPlugin& Instance();

    IPluginItem* GetItem(int index) override;
    void DataRequired() override;
    OptionReturn ShowOptionsDialog(void* hParent) override;
    const wchar_t* GetInfo(PluginInfoIndex index) override;
    const wchar_t* GetTooltipInfo() override;
    void OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data) override;
    void OnInitialize(ITrafficMonitor* app) override;
    int GetCommandCount() override { return 2; }
    const wchar_t* GetCommandName(int index) override;
    void OnPluginCommand(int index, void* hWnd, void* para) override;

    // 在默认浏览器中打开 CPA 管理页面。
    void OpenManagementPage();

private:
    CpaPlugin();

    void EnsureConfigLoaded();
    std::wstring ConfigPath() const;
    void StartFetchIfDue(bool force);
    void NotifyLowQuota(const Snapshot& snapshot);
    std::wstring BuildTooltip() const;

    ITrafficMonitor* app_{};
    std::wstring config_dir_;
    bool config_loaded_ = false;
    Config config_;

    // 以下成员只在主线程访问。
    std::array<QuotaItem, 6> items_;
    std::shared_ptr<const Snapshot> shown_;
    std::wstring tooltip_;
    int64_t last_fetch_ = 0;
    bool paused_ = false;   // 管理密钥被拒绝后暂停自动刷新
    LowQuotaAlerts alerts_;

    // 后台线程与主线程之间的交接区。
    std::mutex mutex_;
    bool fetching_ = false;
    uint64_t generation_ = 0;
    std::shared_ptr<const Snapshot> pending_;
};

} // namespace cpa

extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance();
