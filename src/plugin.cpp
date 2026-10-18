#include "plugin.h"

#include <windows.h>

#include <algorithm>
#include <ctime>
#include <thread>

#include "config.h"
#include "http_client.h"
#include "options_dialog.h"
#include "version.h"

namespace cpa {

namespace {

constexpr wchar_t kIniName[] = L"CPAQuota.ini";

const QuotaWindow& WindowOf(const AccountQuota& q, WindowKind kind)
{
    return kind == WindowKind::FiveHour ? q.five_hour : q.weekly;
}

std::wstring FormatPercent(double remaining)
{
    if (remaining < 0) return L"--";
    return std::to_wstring(static_cast<int>(remaining + 0.5)) + L"%";
}

std::wstring FormatWindow(const QuotaWindow& w, const wchar_t* name, int64_t now)
{
    if (!w.present) return {};
    std::wstring s = (w.monthly ? L"30d" : name);
    s += L" " + FormatPercent(w.RemainingPercent());
    if (w.reset_at > now) s += L" (" + FormatDuration(w.reset_at - now) + L")";
    return s;
}

std::wstring FormatClock(int64_t t)
{
    std::tm tm{};
    __time64_t tt = t;
    if (_localtime64_s(&tm, &tt) != 0) return {};
    wchar_t buf[16];
    wcsftime(buf, std::size(buf), L"%H:%M:%S", &tm);
    return buf;
}

} // namespace

// ---------------------------------------------------------------------------
// QuotaItem

QuotaItem::QuotaItem(Provider provider, WindowKind window, const wchar_t* name, const wchar_t* id, const wchar_t* label)
    : provider_(provider), window_(window), name_(name), id_(id), label_(label)
{
}

float QuotaItem::GetResourceUsageGraphValue() const
{
    return remaining_ < 0 ? 0.0f : static_cast<float>(remaining_ / 100.0);
}

void QuotaItem::Update(const Snapshot* snapshot)
{
    remaining_ = -1;
    if (snapshot && snapshot->ok) {
        for (const AccountQuota& q : snapshot->accounts) {
            if (q.account.provider != provider_ || !q.ok) continue;
            double r = WindowOf(q, window_).RemainingPercent();
            if (r >= 0 && (remaining_ < 0 || r < remaining_)) remaining_ = r;
        }
    }
    value_ = FormatPercent(remaining_);
}

// ---------------------------------------------------------------------------
// CpaPlugin

CpaPlugin::CpaPlugin()
    : items_{{
          {Provider::Codex, WindowKind::FiveHour, L"CPA Codex 5h 剩余", L"CpaCodex5h", L"X5h: "},
          {Provider::Codex, WindowKind::Weekly, L"CPA Codex 7d 剩余", L"CpaCodexWeek", L"X7d: "},
          {Provider::Claude, WindowKind::FiveHour, L"CPA Claude 5h 剩余", L"CpaClaude5h", L"C5h: "},
          {Provider::Claude, WindowKind::Weekly, L"CPA Claude 7d 剩余", L"CpaClaudeWeek", L"C7d: "},
          {Provider::Antigravity, WindowKind::FiveHour, L"CPA Gemini 5h 剩余", L"CpaGemini5h", L"G5h: "},
          {Provider::Antigravity, WindowKind::Weekly, L"CPA Gemini 7d 剩余", L"CpaGeminiWeek", L"G7d: "},
      }}
{
}

CpaPlugin& CpaPlugin::Instance()
{
    // 有意不释放：后台线程可能在进程退出时仍持有指针。
    static CpaPlugin* instance = new CpaPlugin();
    return *instance;
}

IPluginItem* CpaPlugin::GetItem(int index)
{
    if (index < 0 || index >= static_cast<int>(items_.size())) return nullptr;
    return &items_[static_cast<size_t>(index)];
}

std::wstring CpaPlugin::ConfigPath() const
{
    std::wstring dir = config_dir_.empty() ? ModuleDirectory() : config_dir_;
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
    return dir + kIniName;
}

void CpaPlugin::EnsureConfigLoaded()
{
    if (config_loaded_) return;
    config_ = LoadConfig(ConfigPath());
    config_loaded_ = true;
}

void CpaPlugin::DataRequired()
{
    EnsureConfigLoaded();

    std::shared_ptr<const Snapshot> fresh;
    {
        std::lock_guard lock(mutex_);
        fresh = std::move(pending_);
    }
    if (fresh) {
        shown_ = std::move(fresh);
        if (shown_->auth_rejected) paused_ = true;
        for (QuotaItem& item : items_) item.Update(shown_.get());
    }

    StartFetchIfDue(false);
}

void CpaPlugin::StartFetchIfDue(bool force)
{
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    if (!force) {
        if (paused_) return;
        if (last_fetch_ != 0 && now - last_fetch_ < static_cast<int64_t>(config_.refresh_seconds)) return;
    }

    uint64_t generation;
    {
        std::lock_guard lock(mutex_);
        if (fetching_) return;
        fetching_ = true;
        generation = generation_;
    }
    last_fetch_ = now;
    paused_ = false;

    std::thread([this, config = config_, generation]() {
        auto snapshot = std::make_shared<const Snapshot>(FetchSnapshot(config));
        std::lock_guard lock(mutex_);
        fetching_ = false;
        if (generation == generation_) pending_ = std::move(snapshot);
    }).detach();
}

ITMPlugin::OptionReturn CpaPlugin::ShowOptionsDialog(void* hParent)
{
    EnsureConfigLoaded();
    Config edited = config_;
    if (!cpa::ShowOptionsDialog(static_cast<HWND>(hParent), edited)) return OR_OPTION_UNCHANGED;

    std::wstring error;
    if (!SaveConfig(ConfigPath(), edited, &error))
        MessageBoxW(static_cast<HWND>(hParent), error.c_str(), L"CPAQuota", MB_ICONWARNING);

    config_ = edited;
    {
        std::lock_guard lock(mutex_);
        ++generation_;      // 丢弃旧配置下仍在进行的请求结果
        fetching_ = false;  // 允许立即以新配置发起请求
        pending_.reset();
    }
    shown_.reset();
    for (QuotaItem& item : items_) item.Update(nullptr);
    StartFetchIfDue(true);
    return OR_OPTION_CHANGED;
}

const wchar_t* CpaPlugin::GetInfo(PluginInfoIndex index)
{
    switch (index) {
    case TMI_NAME:        return L"CPA 额度";
    case TMI_DESCRIPTION: return L"通过 CLIProxyAPI 管理接口显示 Codex / Claude / Antigravity 账号的 5 小时与周额度剩余。";
    case TMI_AUTHOR:      return L"霞葉";
    case TMI_COPYRIGHT:   return L"Copyright (C) 2026 霞葉";
    case TMI_VERSION:     return CPA_VERSION_WSTR;
    case TMI_URL:         return L"";
    default:              return L"";
    }
}

std::wstring CpaPlugin::BuildTooltip() const
{
    if (!config_loaded_) return L"CPA 额度：等待数据";
    if (!shown_) return L"CPA 额度：正在获取…";

    const Snapshot& s = *shown_;
    std::wstring tip = L"CPA 额度（更新于 " + FormatClock(s.fetched_at) + L"）";
    if (!s.ok) {
        tip += L"\n" + s.error;
        if (paused_) tip += L"\n已暂停自动刷新，请检查插件选项中的管理密钥";
        return tip;
    }

    int64_t now = static_cast<int64_t>(std::time(nullptr));
    auto windows = [now](const QuotaWindow& five_hour, const QuotaWindow& weekly) {
        std::wstring five = FormatWindow(five_hour, L"5h", now);
        std::wstring week = FormatWindow(weekly, L"7d", now);
        return five + (!five.empty() && !week.empty() ? L" · " : L"") + week;
    };

    for (Provider p : {Provider::Codex, Provider::Claude, Provider::Antigravity}) {
        bool header = false;
        for (const AccountQuota& q : s.accounts) {
            if (q.account.provider != p) continue;
            if (!header) {
                tip += p == Provider::Codex ? L"\n[Codex]" : p == Provider::Claude ? L"\n[Claude]" : L"\n[Antigravity]";
                header = true;
            }
            tip += L"\n  " + Utf8ToWide(q.account.DisplayName());
            if (!q.plan.empty()) tip += L" (" + Utf8ToWide(q.plan) + L")";
            tip += L":";
            if (!q.ok) {
                tip += L" 错误 " + q.error;
            } else if (p == Provider::Antigravity) {
                for (const QuotaGroup& g : q.groups)
                    tip += L"\n    " + Utf8ToWide(g.name) + L": " + windows(g.five_hour, g.weekly);
            } else {
                tip += L" " + windows(q.five_hour, q.weekly);
            }
        }
    }
    if (s.accounts.empty()) tip += L"\nCPA 中没有已启用的 Codex / Claude / Antigravity 账号";
    return tip;
}

const wchar_t* CpaPlugin::GetTooltipInfo()
{
    tooltip_ = BuildTooltip();
    return tooltip_.c_str();
}

void CpaPlugin::OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data)
{
    if (index == EI_CONFIG_DIR && data && *data && config_dir_.empty()) config_dir_ = data;
}

void CpaPlugin::OnInitialize(ITrafficMonitor* app)
{
    app_ = app;
    if (app_ && config_dir_.empty()) {
        if (const wchar_t* dir = app_->GetPluginConfigDir(); dir && *dir) config_dir_ = dir;
    }
}

const wchar_t* CpaPlugin::GetCommandName(int index)
{
    return index == 0 ? L"立即刷新额度" : nullptr;
}

void CpaPlugin::OnPluginCommand(int index, void*, void*)
{
    if (index != 0) return;
    EnsureConfigLoaded();
    StartFetchIfDue(true);
}

} // namespace cpa

ITMPlugin* TMPluginGetInstance()
{
    return &cpa::CpaPlugin::Instance();
}
