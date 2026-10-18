#pragma once

namespace cpa {

// 界面语言：跟随 TrafficMonitor 的语言设置，中文以外一律使用英文。
void SetEnglish(bool english);
bool IsEnglish();

// 按当前语言选择文本。两个参数都应是字符串字面量。
inline const wchar_t* T(const wchar_t* zh, const wchar_t* en)
{
    return IsEnglish() ? en : zh;
}

// 由 LANGID 判断是否使用英文（非中文即英文）。0 表示 TrafficMonitor「跟随系统」，按系统界面语言判断。
bool IsEnglishLanguage(unsigned short langid);

} // namespace cpa
