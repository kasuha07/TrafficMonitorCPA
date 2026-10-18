#include "config.h"

#include <windows.h>
#include <dpapi.h>

#include <vector>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace cpa {

namespace {

constexpr wchar_t kSection[] = L"CPA";

std::wstring ReadString(const std::wstring& ini, const wchar_t* key, const wchar_t* def)
{
    std::wstring buf(4096, L'\0');
    DWORD n = GetPrivateProfileStringW(kSection, key, def, buf.data(), static_cast<DWORD>(buf.size()), ini.c_str());
    buf.resize(n);
    return buf;
}

std::wstring ToHex(const BYTE* data, DWORD size)
{
    static const wchar_t hex[] = L"0123456789abcdef";
    std::wstring out;
    out.reserve(size * 2);
    for (DWORD i = 0; i < size; ++i) {
        out += hex[data[i] >> 4];
        out += hex[data[i] & 0xF];
    }
    return out;
}

bool FromHex(const std::wstring& s, std::vector<BYTE>& out)
{
    if (s.size() % 2) return false;
    auto nibble = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    };
    out.clear();
    for (size_t i = 0; i < s.size(); i += 2) {
        int hi = nibble(s[i]), lo = nibble(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<BYTE>(hi << 4 | lo));
    }
    return true;
}

std::wstring Protect(const std::wstring& plain)
{
    if (plain.empty()) return {};
    DATA_BLOB in{static_cast<DWORD>(plain.size() * sizeof(wchar_t)),
                 reinterpret_cast<BYTE*>(const_cast<wchar_t*>(plain.data()))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"CPAQuota", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};
    std::wstring hex = ToHex(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return hex;
}

std::wstring Unprotect(const std::wstring& hex)
{
    std::vector<BYTE> bytes;
    if (hex.empty() || !FromHex(hex, bytes)) return {};
    DATA_BLOB in{static_cast<DWORD>(bytes.size()), bytes.data()};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};
    std::wstring plain(reinterpret_cast<wchar_t*>(out.pbData), out.cbData / sizeof(wchar_t));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return plain;
}

} // namespace

Config LoadConfig(const std::wstring& ini_path)
{
    Config c;
    std::wstring url = ReadString(ini_path, L"BaseUrl", L"");
    if (!url.empty()) c.base_url = url;
    c.management_key = Unprotect(ReadString(ini_path, L"ManagementKey", L""));
    UINT refresh = GetPrivateProfileIntW(kSection, L"RefreshSeconds", 300, ini_path.c_str());
    c.refresh_seconds = refresh < 15 ? 15 : (refresh > 3600 ? 3600 : refresh);
    c.enable_codex = GetPrivateProfileIntW(kSection, L"EnableCodex", 0, ini_path.c_str()) != 0;
    c.enable_claude = GetPrivateProfileIntW(kSection, L"EnableClaude", 0, ini_path.c_str()) != 0;
    c.enable_antigravity = GetPrivateProfileIntW(kSection, L"EnableAntigravity", 0, ini_path.c_str()) != 0;
    UINT aggregate = GetPrivateProfileIntW(kSection, L"Aggregate", 0, ini_path.c_str());
    c.aggregate = aggregate <= 3 ? static_cast<AggregateMode>(aggregate) : AggregateMode::Min;
    c.double_click = GetPrivateProfileIntW(kSection, L"DoubleClick", 0, ini_path.c_str()) == 1
                         ? DoubleClickAction::Refresh
                         : DoubleClickAction::OpenManagementPage;
    c.smart_cooldown = GetPrivateProfileIntW(kSection, L"SmartCooldown", 1, ini_path.c_str()) != 0;
    c.skip_tls_verify = GetPrivateProfileIntW(kSection, L"SkipTlsVerify", 0, ini_path.c_str()) != 0;
    c.show_used = GetPrivateProfileIntW(kSection, L"ShowUsed", 0, ini_path.c_str()) != 0;
    c.show_countdown = GetPrivateProfileIntW(kSection, L"ShowCountdown", 0, ini_path.c_str()) != 0;
    c.color_by_quota = GetPrivateProfileIntW(kSection, L"ColorByQuota", 0, ini_path.c_str()) != 0;
    UINT alert = GetPrivateProfileIntW(kSection, L"AlertThreshold", 10, ini_path.c_str());
    c.alert_threshold = alert > 99 ? 99 : alert;
    return c;
}

bool SaveConfig(const std::wstring& ini_path, const Config& config, std::wstring* error)
{
    std::wstring key_hex = Protect(config.management_key);
    if (!config.management_key.empty() && key_hex.empty()) {
        if (error) *error = L"加密管理密钥失败";
        return false;
    }
    std::wstring refresh = std::to_wstring(config.refresh_seconds);
    auto flag = [](bool b) { return b ? L"1" : L"0"; };
    bool ok = WritePrivateProfileStringW(kSection, L"BaseUrl", config.base_url.c_str(), ini_path.c_str()) &&
              WritePrivateProfileStringW(kSection, L"ManagementKey", key_hex.c_str(), ini_path.c_str()) &&
              WritePrivateProfileStringW(kSection, L"RefreshSeconds", refresh.c_str(), ini_path.c_str()) &&
              WritePrivateProfileStringW(kSection, L"EnableCodex", flag(config.enable_codex), ini_path.c_str()) &&
              WritePrivateProfileStringW(kSection, L"EnableClaude", flag(config.enable_claude), ini_path.c_str()) &&
              WritePrivateProfileStringW(kSection, L"EnableAntigravity", flag(config.enable_antigravity), ini_path.c_str()) &&
              WritePrivateProfileStringW(kSection, L"Aggregate", std::to_wstring(static_cast<int>(config.aggregate)).c_str(),
                                         ini_path.c_str()) &&
              WritePrivateProfileStringW(kSection, L"AlertThreshold", std::to_wstring(config.alert_threshold).c_str(),
                                         ini_path.c_str()) &&
              WritePrivateProfileStringW(kSection, L"ShowUsed", flag(config.show_used), ini_path.c_str()) &&
              WritePrivateProfileStringW(kSection, L"ShowCountdown", flag(config.show_countdown), ini_path.c_str()) &&
              WritePrivateProfileStringW(kSection, L"ColorByQuota", flag(config.color_by_quota), ini_path.c_str()) &&
              WritePrivateProfileStringW(kSection, L"SkipTlsVerify", flag(config.skip_tls_verify), ini_path.c_str()) &&
              WritePrivateProfileStringW(kSection, L"SmartCooldown", flag(config.smart_cooldown), ini_path.c_str()) &&
              WritePrivateProfileStringW(kSection, L"DoubleClick",
                                         std::to_wstring(static_cast<int>(config.double_click)).c_str(), ini_path.c_str());
    if (!ok && error) *error = L"无法写入配置文件: " + ini_path;
    return ok;
}

std::wstring ModuleDirectory()
{
    wchar_t path[MAX_PATH]{};
    DWORD n = GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), path, MAX_PATH);
    std::wstring dir(path, n);
    size_t slash = dir.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : dir.substr(0, slash + 1);
}

} // namespace cpa
