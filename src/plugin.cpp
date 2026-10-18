#include "plugin.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <ctime>
#include <thread>

#include "config.h"
#include "http_client.h"
#include "i18n.h"
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

std::wstring FormatWindow(const QuotaWindow& w, const wchar_t* name, int64_t now, bool used)
{
    if (!w.present) return {};
    std::wstring s = (w.monthly ? L"30d" : name);
    double remaining = w.RemainingPercent();
    s += L" " + FormatPercent(used && remaining >= 0 ? 100.0 - remaining : remaining);
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

int QuotaItem::OnMouseEvent(MouseEventType type, int, int, void*, int)
{
    if (type != MT_DBCLICKED) return 0;
    CpaPlugin::Instance().OnItemDoubleClick();
    return 1;  // 已处理，主程序不再执行默认的双击动作
}

unsigned long QuotaColor(double remaining_percent, bool dark_mode, unsigned long fallback)
{
    if (remaining_percent < 0) return fallback;
    if (remaining_percent < 10.5) return dark_mode ? RGB(255, 110, 110) : RGB(215, 40, 40);
    if (remaining_percent < 30.5) return dark_mode ? RGB(255, 200, 70) : RGB(200, 130, 0);
    return fallback;
}

bool QuotaItem::IsCustomDraw() const
{
    return CpaPlugin::Instance().ColorEnabled();
}

int QuotaItem::GetItemWidthEx(void* hDC) const
{
    std::wstring text = std::wstring(label_) + sample_;
    SIZE size{};
    if (!GetTextExtentPoint32W(static_cast<HDC>(hDC), text.c_str(), static_cast<int>(text.size()), &size)) return 0;
    return size.cx;
}

namespace {

// 按剩余比例着色时，数值颜色取决于剩余比例；资源占用图的值即剩余比例（账号数模式为可用比例）。
void ResolveColors(double graph, bool dark_mode, unsigned long& label, unsigned long& value)
{
    const DrawColors& c = CpaPlugin::Instance().Colors();
    unsigned long fallback = dark_mode ? RGB(255, 255, 255) : RGB(0, 0, 0);
    label = c.known ? c.label : fallback;
    unsigned long base = c.known ? c.value : fallback;
    value = QuotaColor(graph < 0 ? -1 : graph * 100.0, dark_mode, base);
}

} // namespace

void QuotaItem::DrawItem(void* hDC, int x, int y, int w, int h, bool dark_mode)
{
    if (drawn_ex_) return;  // 新版 TrafficMonitor 随后会调用 DrawItemEx
    HDC dc = static_cast<HDC>(hDC);
    unsigned long label_color = 0, value_color = 0;
    ResolveColors(graph_, dark_mode, label_color, value_color);

    RECT rect{x, y, x + w, y + h};
    int old_mode = SetBkMode(dc, TRANSPARENT);
    COLORREF old_color = SetTextColor(dc, label_color);
    DrawTextW(dc, label_, -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SIZE label_size{};
    GetTextExtentPoint32W(dc, label_, static_cast<int>(wcslen(label_)), &label_size);
    rect.left += label_size.cx;
    SetTextColor(dc, value_color);
    DrawTextW(dc, value_.c_str(), -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SetTextColor(dc, old_color);
    SetBkMode(dc, old_mode);
}

bool QuotaItem::DrawItemEx(IPluginDrawer* drawer, int x, int y, int w, int h, bool dark_mode)
{
    if (!drawer) return false;
    drawn_ex_ = true;
    unsigned long label_color = 0, value_color = 0;
    ResolveColors(graph_, dark_mode, label_color, value_color);

    int label_w = 0, label_h = 0;
    drawer->GetTextExtent(label_, label_w, label_h);
    drawer->DrawWindowText(x, y, label_w, h, label_, label_color);
    drawer->DrawWindowText(x + label_w, y, w - label_w, h, value_.c_str(), value_color);
    return true;
}

float QuotaItem::GetResourceUsageGraphValue() const
{
    return graph_ < 0 ? 0.0f : static_cast<float>(graph_);
}

void QuotaItem::Update(const Snapshot* snapshot, const Config& config)
{
    const AggregateMode mode = config.aggregate;
    sample_ = config.show_countdown ? L"100% 23h" : L"100%";

    std::vector<double> remaining;
    std::vector<int64_t> resets;
    if (snapshot && snapshot->ok) {
        for (const AccountQuota& q : snapshot->accounts) {
            if (q.account.provider != provider_ || !q.ok) continue;
            const QuotaWindow& w = WindowOf(q, window_);
            remaining.push_back(w.RemainingPercent());
            resets.push_back(w.reset_at);
        }
    }

    AggregateResult r = Aggregate(remaining, mode);
    if (r.total == 0) {
        graph_ = -1;
        value_ = L"--";
        return;
    }
    if (mode == AggregateMode::AvailableCount) {
        graph_ = static_cast<double>(r.available) / r.total;
        value_ = std::to_wstring(r.available) + L"/" + std::to_wstring(r.total);
    } else {
        // 资源占用图始终表示剩余比例，只有文字随「已用」设置变化。
        graph_ = r.percent / 100.0;
        value_ = FormatPercent(config.show_used ? 100.0 - r.percent : r.percent);
    }

    if (config.show_countdown) {
        // 最低/最高取所显示账号的重置时间；平均/账号数取最早的重置时间。
        const int64_t now = static_cast<int64_t>(std::time(nullptr));
        int64_t reset = 0;
        if (r.index >= 0) {
            reset = resets[static_cast<size_t>(r.index)];
        } else {
            for (size_t i = 0; i < resets.size(); ++i) {
                if (remaining[i] >= 0 && resets[i] > now && (reset == 0 || resets[i] < reset)) reset = resets[i];
            }
        }
        if (reset > now) value_ += L" " + FormatDurationShort(reset - now);
    }
}

// ---------------------------------------------------------------------------
// CpaPlugin

CpaPlugin::CpaPlugin()
    : items_{{
          {Provider::Codex, WindowKind::FiveHour, L"CPA Codex 5h", L"CpaCodex5h", L"X5h: "},
          {Provider::Codex, WindowKind::Weekly, L"CPA Codex 7d", L"CpaCodexWeek", L"X7d: "},
          {Provider::Claude, WindowKind::FiveHour, L"CPA Claude 5h", L"CpaClaude5h", L"C5h: "},
          {Provider::Claude, WindowKind::Weekly, L"CPA Claude 7d", L"CpaClaudeWeek", L"C7d: "},
          {Provider::Antigravity, WindowKind::FiveHour, L"CPA Gemini 5h", L"CpaGemini5h", L"G5h: "},
          {Provider::Antigravity, WindowKind::Weekly, L"CPA Gemini 7d", L"CpaGeminiWeek", L"G7d: "},
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
        for (QuotaItem& item : items_) item.Update(shown_.get(), config_);
        if (shown_->ok) NotifyLowQuota(*shown_);
    }

    StartFetchIfDue(false);
}

void CpaPlugin::NotifyLowQuota(const Snapshot& snapshot)
{
    auto messages = alerts_.Check(snapshot.accounts, static_cast<int>(config_.alert_threshold),
                                  static_cast<int64_t>(std::time(nullptr)));
    if (messages.empty() || !app_) return;
    std::wstring text = T(L"CPA 额度不足", L"CPA quota low");
    for (const std::wstring& m : messages) text += L"\n" + m;
    app_->ShowNotifyMessage(text.c_str());
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

    // 上一次的结果用于智能冷却；手动刷新（force）时忽略冷却。
    std::thread([this, config = config_, generation, previous = shown_, force]() {
        auto snapshot = std::make_shared<const Snapshot>(FetchSnapshot(config, previous.get(), force));
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
    for (QuotaItem& item : items_) item.Update(nullptr, config_);
    StartFetchIfDue(true);
    return OR_OPTION_CHANGED;
}

const wchar_t* CpaPlugin::GetInfo(PluginInfoIndex index)
{
    switch (index) {
    case TMI_NAME:        return T(L"CPA 额度", L"CPA Quota");
    case TMI_DESCRIPTION: return T(L"通过 CLIProxyAPI 管理接口显示 Codex / Claude / Antigravity 账号的 5 小时与周额度剩余。", L"Shows 5-hour and weekly quota of Codex / Claude / Antigravity accounts via the CLIProxyAPI management API.");
    case TMI_AUTHOR:      return L"霞葉";
    case TMI_COPYRIGHT:   return L"Copyright (C) 2026 霞葉";
    case TMI_VERSION:     return CPA_VERSION_WSTR;
    case TMI_URL:         return L"";
    default:              return L"";
    }
}

std::wstring CpaPlugin::BuildTooltip() const
{
    if (!config_loaded_) return T(L"CPA 额度：等待数据", L"CPA Quota: waiting for data");
    if (!shown_) return T(L"CPA 额度：正在获取…", L"CPA Quota: loading…");

    const Snapshot& s = *shown_;
    std::wstring tip = std::wstring(config_.show_used ? T(L"CPA 额度已用", L"CPA quota used") : T(L"CPA 额度", L"CPA quota")) +
                       T(L"（更新于 ", L" (updated ") + FormatClock(s.fetched_at) + T(L"）", L")");
    if (!s.ok) {
        tip += L"\n" + s.error;
        if (paused_) tip += T(L"\n已暂停自动刷新，请检查插件选项中的管理密钥", L"\nAuto refresh paused. Check the management key in plugin options.");
        return tip;
    }

    int64_t now = static_cast<int64_t>(std::time(nullptr));
    if (s.failures > 0) {
        tip += T(L"\n本次刷新失败（连续 ", L"\nRefresh failed (") + std::to_wstring(s.failures) +
               T(L" 次），显示 ", L" in a row), showing data from ") + FormatDuration(now - s.fetched_at) +
               T(L" 前的数据：", L" ago: ") + s.warning;
    }
    const bool used = config_.show_used;
    auto stale_note = [now](const AccountQuota& q) -> std::wstring {
        if (!q.stale) return {};
        std::wstring note = T(L" · 沿用 ", L" · data from ") + FormatDuration(now - q.data_at) + T(L" 前数据", L" ago");
        if (!q.error.empty()) note += T(L"（", L" (") + q.error + T(L"）", L")");
        return note;
    };
    auto windows = [now, used](const QuotaWindow& five_hour, const QuotaWindow& weekly) {
        std::wstring five = FormatWindow(five_hour, L"5h", now, used);
        std::wstring week = FormatWindow(weekly, L"7d", now, used);
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
                tip += T(L" 错误 ", L" error: ") + q.error;
            } else if (p == Provider::Antigravity) {
                if (q.cooled) tip += T(L" 冷却中", L" cooling down");
                tip += stale_note(q);
                for (const QuotaGroup& g : q.groups)
                    tip += L"\n    " + Utf8ToWide(g.name) + L": " + windows(g.five_hour, g.weekly);
            } else {
                tip += L" " + windows(q.five_hour, q.weekly);
                const ResetCredits& rc = q.reset_credits;
                if (rc.available > 0) {
                    tip += T(L" · 重置卡*", L" · reset cards*") + std::to_wstring(rc.available);
                    if (rc.soonest_expiry > now) tip += L" (" + FormatDuration(rc.soonest_expiry - now) + L")";
                }
                if (q.cooled) tip += T(L" · 冷却中", L" · cooling down");
                tip += stale_note(q);
            }
        }
    }
    if (s.accounts.empty())
        tip += T(L"\nCPA 中没有可查询的账号（检查账号是否启用，以及插件选项中勾选的账号类型）",
                 L"\nNo accounts to query (check that accounts are enabled in CPA and account types are selected in options)");
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
    if (index == EI_LABEL_TEXT_COLOR && data) {
        colors_.label = static_cast<unsigned long>(_wtoi64(data));
        colors_.known = true;
    }
    if (index == EI_VALUE_TEXT_COLOR && data) colors_.value = static_cast<unsigned long>(_wtoi64(data));
}

void CpaPlugin::OnInitialize(ITrafficMonitor* app)
{
    app_ = app;
    if (app_) SetEnglish(IsEnglishLanguage(app_->GetLanguageId()));
    if (app_ && config_dir_.empty()) {
        if (const wchar_t* dir = app_->GetPluginConfigDir(); dir && *dir) config_dir_ = dir;
    }
}

const wchar_t* CpaPlugin::GetCommandName(int index)
{
    switch (index) {
    case 0: return T(L"立即刷新额度", L"Refresh quota now");
    case 1: return T(L"打开 CPA 管理页面", L"Open CPA management page");
    default: return nullptr;
    }
}

void CpaPlugin::OnPluginCommand(int index, void*, void*)
{
    EnsureConfigLoaded();
    if (index == 0) StartFetchIfDue(true);
    else if (index == 1) OpenManagementPage();
}

void CpaPlugin::OnItemDoubleClick()
{
    EnsureConfigLoaded();
    if (config_.double_click == DoubleClickAction::Refresh) StartFetchIfDue(true);
    else OpenManagementPage();
}

void CpaPlugin::OpenManagementPage()
{
    EnsureConfigLoaded();
    std::wstring url = config_.base_url;
    // 只打开 http(s) 地址，避免把配置内容当作任意命令交给 ShellExecute。
    if (url.rfind(L"http://", 0) != 0 && url.rfind(L"https://", 0) != 0) return;
    while (!url.empty() && url.back() == L'/') url.pop_back();
    url += L"/management.html";
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

} // namespace cpa

ITMPlugin* TMPluginGetInstance()
{
    return &cpa::CpaPlugin::Instance();
}
