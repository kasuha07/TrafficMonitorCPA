#include "plugin.h"

#include <windows.h>

#include "version.h"

namespace cpa {

const wchar_t* CpaItem::GetItemName() const { return L"CPA"; }
const wchar_t* CpaItem::GetItemId() const { return L"cpa_main"; }
const wchar_t* CpaItem::GetItemLableText() const { return L"CPA:"; }
const wchar_t* CpaItem::GetItemValueText() const { return value_.c_str(); }
const wchar_t* CpaItem::GetItemValueSampleText() const { return L"00:00:00"; }

CpaPlugin& CpaPlugin::Instance()
{
    static CpaPlugin instance;
    return instance;
}

IPluginItem* CpaPlugin::GetItem(int index)
{
    return index == 0 ? &item_ : nullptr;
}

void CpaPlugin::DataRequired()
{
    // TODO: 替换为实际的数据采集逻辑。占位实现：显示当前时间。
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[16]{};
    swprintf_s(buf, L"%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
    item_.SetValue(buf);
}

const wchar_t* CpaPlugin::GetInfo(PluginInfoIndex index)
{
    switch (index)
    {
    case TMI_NAME:        return L"TrafficMonitorCPA";
    case TMI_DESCRIPTION: return L"Lightweight TrafficMonitor plugin";
    case TMI_AUTHOR:      return L"kasuha";
    case TMI_COPYRIGHT:   return L"Copyright (C) 2026";
    case TMI_VERSION:     return CPA_VERSION_WSTR;
    case TMI_URL:         return L"";
    default:              return L"";
    }
}

void CpaPlugin::OnInitialize(ITrafficMonitor* app)
{
    app_ = app;
}

} // namespace cpa

ITMPlugin* TMPluginGetInstance()
{
    return &cpa::CpaPlugin::Instance();
}
