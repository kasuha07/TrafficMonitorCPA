#include "i18n.h"

#include <windows.h>

#include <atomic>

namespace cpa {

namespace {

// 在 TrafficMonitor 传入语言之前，先跟随系统界面语言。
std::atomic<bool> g_english{IsEnglishLanguage(GetUserDefaultUILanguage())};

} // namespace

void SetEnglish(bool english)
{
    g_english = english;
}

bool IsEnglish()
{
    return g_english;
}

bool IsEnglishLanguage(unsigned short langid)
{
    // TrafficMonitor 的语言设置为「跟随系统」时传入 0，此时按系统界面语言判断。
    if (langid == 0) langid = GetUserDefaultUILanguage();
    return PRIMARYLANGID(langid) != LANG_CHINESE;
}

} // namespace cpa
