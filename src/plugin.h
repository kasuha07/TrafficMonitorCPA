#pragma once

#include <string>

#include "PluginInterface.h"

namespace cpa {

// 单个显示项目。数值在 DataRequired 中刷新，Get* 只返回缓存的字符串。
class CpaItem final : public IPluginItem
{
public:
    const wchar_t* GetItemName() const override;
    const wchar_t* GetItemId() const override;
    const wchar_t* GetItemLableText() const override;
    const wchar_t* GetItemValueText() const override;
    const wchar_t* GetItemValueSampleText() const override;

    void SetValue(std::wstring value) { value_ = std::move(value); }

private:
    std::wstring value_{L"--"};
};

class CpaPlugin final : public ITMPlugin
{
public:
    static CpaPlugin& Instance();

    IPluginItem* GetItem(int index) override;
    void DataRequired() override;
    const wchar_t* GetInfo(PluginInfoIndex index) override;
    void OnInitialize(ITrafficMonitor* app) override;

private:
    CpaPlugin() = default;

    ITrafficMonitor* app_{};
    CpaItem item_;
};

} // namespace cpa

extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance();
