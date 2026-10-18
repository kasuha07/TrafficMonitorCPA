#include "options_dialog.h"

#include <string>

#include "resource.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace cpa {

namespace {

std::wstring GetText(HWND dlg, int id)
{
    HWND ctrl = GetDlgItem(dlg, id);
    int len = GetWindowTextLengthW(ctrl);
    std::wstring s(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(ctrl, s.data(), len + 1);
    s.resize(static_cast<size_t>(len));
    size_t b = s.find_first_not_of(L" \t");
    size_t e = s.find_last_not_of(L" \t");
    return b == std::wstring::npos ? std::wstring() : s.substr(b, e - b + 1);
}

// 读取并校验对话框中的输入；失败时在状态栏显示原因。
bool ReadInputs(HWND dlg, Config& out)
{
    out.base_url = GetText(dlg, IDC_URL);
    out.management_key = GetText(dlg, IDC_KEY);
    std::wstring refresh = GetText(dlg, IDC_REFRESH);

    if (out.base_url.rfind(L"http://", 0) != 0 && out.base_url.rfind(L"https://", 0) != 0) {
        SetDlgItemTextW(dlg, IDC_STATUS, L"CPA 地址需以 http:// 或 https:// 开头");
        return false;
    }
    unsigned seconds = refresh.empty() ? 0 : static_cast<unsigned>(_wtoi(refresh.c_str()));
    if (seconds < 15 || seconds > 3600) {
        SetDlgItemTextW(dlg, IDC_STATUS, L"刷新间隔需在 15~3600 秒之间");
        return false;
    }
    out.refresh_seconds = seconds;

    out.enable_codex = IsDlgButtonChecked(dlg, IDC_ENABLE_CODEX) == BST_CHECKED;
    out.enable_claude = IsDlgButtonChecked(dlg, IDC_ENABLE_CLAUDE) == BST_CHECKED;
    out.enable_antigravity = IsDlgButtonChecked(dlg, IDC_ENABLE_ANTIGRAVITY) == BST_CHECKED;
    if (!out.enable_codex && !out.enable_claude && !out.enable_antigravity) {
        SetDlgItemTextW(dlg, IDC_STATUS, L"至少需要启用一种账号类型");
        return false;
    }

    LRESULT sel = SendDlgItemMessageW(dlg, IDC_AGGREGATE, CB_GETCURSEL, 0, 0);
    out.aggregate = sel >= 0 && sel <= 3 ? static_cast<AggregateMode>(sel) : AggregateMode::Min;

    std::wstring alert = GetText(dlg, IDC_ALERT);
    unsigned threshold = alert.empty() ? 0 : static_cast<unsigned>(_wtoi(alert.c_str()));
    if (threshold > 99) {
        SetDlgItemTextW(dlg, IDC_STATUS, L"低额度提醒阈值需在 0~99 之间");
        return false;
    }
    out.alert_threshold = threshold;
    return true;
}

INT_PTR CALLBACK DialogProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        auto* config = reinterpret_cast<Config*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        SetDlgItemTextW(dlg, IDC_URL, config->base_url.c_str());
        SetDlgItemTextW(dlg, IDC_KEY, config->management_key.c_str());
        SetDlgItemTextW(dlg, IDC_REFRESH, std::to_wstring(config->refresh_seconds).c_str());
        CheckDlgButton(dlg, IDC_ENABLE_CODEX, config->enable_codex ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_ENABLE_CLAUDE, config->enable_claude ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_ENABLE_ANTIGRAVITY, config->enable_antigravity ? BST_CHECKED : BST_UNCHECKED);
        // 顺序与 AggregateMode 的取值一致。
        for (const wchar_t* text : {L"最低（剩余最少的账号）", L"平均", L"最高（剩余最多的账号）", L"可用账号数（如 3/5）"})
            SendDlgItemMessageW(dlg, IDC_AGGREGATE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        SendDlgItemMessageW(dlg, IDC_AGGREGATE, CB_SETCURSEL, static_cast<WPARAM>(config->aggregate), 0);
        SetDlgItemTextW(dlg, IDC_ALERT, std::to_wstring(config->alert_threshold).c_str());
        SetDlgItemTextW(dlg, IDC_STATUS,
                        L"管理密钥即 CPA 配置中的 remote-management.secret-key。\r\n"
                        L"注意：CPA 对连续 5 次密钥错误会封禁 IP 30 分钟。");
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_TEST: {
            Config probe;
            if (!ReadInputs(dlg, probe)) return TRUE;
            SetDlgItemTextW(dlg, IDC_STATUS, L"正在连接…");
            HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
            UpdateWindow(dlg);
            std::wstring result = TestConnection(probe);
            SetCursor(old);
            SetDlgItemTextW(dlg, IDC_STATUS, result.c_str());
            return TRUE;
        }
        case IDOK: {
            auto* config = reinterpret_cast<Config*>(GetWindowLongPtrW(dlg, DWLP_USER));
            Config updated = *config;  // 保留对话框之外的字段
            if (!ReadInputs(dlg, updated)) return TRUE;
            *config = updated;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

} // namespace

bool ShowOptionsDialog(HWND parent, Config& config)
{
    Config working = config;
    INT_PTR r = DialogBoxParamW(reinterpret_cast<HINSTANCE>(&__ImageBase), MAKEINTRESOURCEW(IDD_OPTIONS), parent,
                                DialogProc, reinterpret_cast<LPARAM>(&working));
    if (r != IDOK) return false;
    config = working;
    return true;
}

} // namespace cpa
