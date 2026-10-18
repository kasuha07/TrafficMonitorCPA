#pragma once

#include <string>

#include "cpa_client.h"

namespace cpa {

// 配置保存在 ini 文件中；管理密钥用 DPAPI（当前用户）加密后以十六进制存储。
Config LoadConfig(const std::wstring& ini_path);
bool SaveConfig(const std::wstring& ini_path, const Config& config, std::wstring* error);

// 本 DLL 所在目录（末尾带反斜杠）。
std::wstring ModuleDirectory();

} // namespace cpa
