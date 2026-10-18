#pragma once

#include <windows.h>

#include "cpa_client.h"

namespace cpa {

// 显示模态选项对话框。用户点击"确定"时返回 true 并写回 config。
bool ShowOptionsDialog(HWND parent, Config& config);

} // namespace cpa
