# CPAQuota

[TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor) 插件：通过 [CLIProxyAPI](https://github.com/router-for-me/CLIProxyAPI)（CPA）的管理接口，在任务栏显示 CPA 中各账号的额度剩余。目前支持 **Codex**、**Claude** 和 **Antigravity（Gemini）** 账号的 5 小时额度和周额度。

## 显示项目

| 标签 | 内容 |
|---|---|
| `X5h` / `X7d` | 所有 Codex 账号中 5 小时 / 7 天额度剩余百分比（默认取**最低**值，可在设置中改为平均 / 最高 / 可用账号数） |
| `C5h` / `C7d` | 所有 Claude 账号中 5 小时 / 7 天额度剩余百分比（默认取**最低**值，可在设置中改为平均 / 最高 / 可用账号数） |
| `G5h` / `G7d` | 所有 Antigravity 账号 **Gemini Models** 分组中 5 小时 / 7 天额度剩余百分比（默认取**最低**值，可在设置中改为平均 / 最高 / 可用账号数） |

- 每个项目都带资源占用图（剩余比例）。
- 鼠标提示列出每个账号的套餐、各窗口剩余百分比和重置倒计时，以及获取失败的原因。Claude 套餐（Team / Max / Pro / Free）来自 `oauth/profile`，每个账号最多 1 小时查询一次。
- Codex / Claude 账号有可用的额度重置卡时，提示中显示为「重置卡*数量 (最早一张的过期倒计时)」：
  - Codex 来自 `wham/rate-limit-reset-credits`，该接口失败时退回用量响应中的数量。
  - Claude 来自 `oauth/usage?cedar_ember=1` 的 reset grants，数量为各 grant 剩余次数之和；为避免触发限流，每个账号最多 10 分钟查询一次。
- 双击任一显示项，或使用插件命令「打开 CPA 管理页面」，在浏览器中打开 `<CPA 地址>/management.html`。
- 插件命令「立即刷新额度」可手动刷新。Codex 团队账号的次级窗口是月度额度，提示中显示为「30d」，任务栏上仍计入 `X7d`。
- Antigravity 按模型分组计算额度（如 Gemini、Claude/GPT），鼠标提示中逐组列出；任务栏只汇总 Gemini 分组（没有该分组时取第一个分组）。账号后显示套餐（Free / Pro / Ultra / Ultra Lite，来自 `loadCodeAssist`，每个账号最多 1 小时查询一次）。

## 工作原理

与 [Cli-Proxy-API-Management-Center](https://github.com/router-for-me/Cli-Proxy-API-Management-Center) 的额度页相同：

1. `GET /v8/management/credentials` 获取账号列表（跳过已禁用的账号）。
2. 对每个账号调用 `POST /v8/management/requests/api-call`，由 CPA 注入该账号的 token，请求上游用量接口：
   - Codex：`https://chatgpt.com/backend-api/wham/usage`
   - Claude：`https://api.anthropic.com/api/oauth/usage`
   - Antigravity：`POST .../v1internal:retrieveUserQuotaSummary`（请求体 `{"project": <project_id>}`，依次尝试 daily / sandbox / 正式三个端点）
3. 解析 Codex 的 `rate_limit.primary_window/secondary_window`、Claude 的 `five_hour/seven_day`、Antigravity 的 `groups[].buckets[]`（`window` + `remainingFraction`）。

插件只需要 CPA 地址和管理密钥，token 始终由 CPA 注入。唯一的例外：Antigravity 账号在账号列表中没有 `project_id` 时，插件会通过 `/v8/management/credentials/download` 读取该账号的凭证文件，只取其中的 `project_id` 并缓存在内存中。

## 设置

在 TrafficMonitor 的插件管理中打开「CPA 额度」的选项：

- **CPA 地址**：默认 `http://127.0.0.1:8317`。
- **管理密钥**：CPA 配置中的 `remote-management.secret-key`。远程访问 CPA 时还需开启 `remote-management.allow-remote`。
- **刷新间隔**：15~3600 秒，默认 300 秒（5 分钟）。
- **查询账号**：分别勾选是否查询 Codex / Claude / Antigravity 账号，**默认全部不勾选**，首次使用需至少勾选一种。未勾选的类型不会请求上游，对应显示项保持 `--`。
- **显示格式**：
  - 「显示已用」：任务栏和鼠标提示显示已用百分比而不是剩余百分比；资源占用图仍表示剩余比例，低额度提醒仍按剩余判断。
  - 「附带重置倒计时」：任务栏数值后加上重置倒计时，如 `82% 4h`。汇总方式为最低 / 最高时取所显示账号的重置时间，平均 / 可用账号数时取最早的重置时间。
- **智能冷却**（默认开启）：某账号的 5h 或 7d 额度用尽且已知重置时间时，自动刷新会跳过该账号直到重置，沿用上次的数据（已过重置时间的窗口显示为已重置），鼠标提示中标注「冷却中」。Antigravity 只有所有模型分组都用尽时才冷却。手动「立即刷新额度」不受冷却影响，可用于在使用重置卡后立即更新。
- **跳过 HTTPS 证书校验**：CPA 使用自签名证书、证书过期或主机名不匹配时勾选。只影响插件到 CPA 的连接，会让连接失去防中间人保护，公网环境下不建议开启；CPA 请求上游时仍正常校验。
- **低额度提醒**：任一账号的任一窗口剩余低于该百分比（默认 10%，0 为关闭）时，通过 TrafficMonitor 弹出通知。同一窗口只提醒一次，剩余回升到阈值以上（通常是额度重置）后再次生效。
- **多账号汇总**：同类有多个账号时，任务栏显示项取「最低」（默认）、「平均」、「最高」，或「可用账号数」（如 `3/5`，即 5 个账号中有 3 个该窗口还有余量）。

「测试连接」会列出 CPA 中可用的 Codex / Claude / Antigravity 账号数。

配置保存在 TrafficMonitor 插件配置目录（取不到时为 DLL 所在目录）的 `CPAQuota.ini`，管理密钥用 Windows DPAPI 按当前用户加密。

> CPA 对同一 IP 连续 5 次管理密钥错误会封禁 30 分钟。插件收到 401/403 后会暂停自动刷新，修改设置或手动刷新后才会再次请求。

## 环境要求

- Visual Studio 生成工具 2026（MSVC v145 / 14.50），Windows SDK 10.0.26100
- CMake ≥ 3.25（需支持 `Visual Studio 18 2026` 生成器）

## 构建与测试

```bash
cmake --preset x64
```

```bash
cmake --build --preset x64-release
```

```bash
ctest --test-dir build/x64 -C Release --output-on-failure
```

32 位版本使用 `x86` / `x86-release` 预设。产物位于 `build/<arch>/<Config>/CPAQuota.dll`，只依赖系统 DLL（静态链接 CRT）。

## CI 与发布

GitHub Actions（`.github/workflows/build.yml`）只在发布 Release 时运行（也可在 Actions 页手动触发一次编译，产物在运行页下载），普通 push 不会触发。

在 GitHub 上发布 Release（标签形如 `v1.2.3`）后，工作流会以该版本号编译，并把 `CPAQuota-1.2.3-x64.zip`、`CPAQuota-1.2.3-x86.zip` 附加到该 Release。GitHub 会在 Release 页面为每个附件显示 SHA-256 摘要，下载后可这样校验：

```bash
Get-FileHash CPAQuota-1.2.3-x64.zip -Algorithm SHA256
```

本地构建和手动触发的 CI 使用 `CMakeLists.txt` 中的默认版本（`CPA_VERSION`），可用 `-DCPA_VERSION=x.y.z` 覆盖。

## 安装

将与 TrafficMonitor 位数一致的 `CPAQuota.dll` 复制到 TrafficMonitor 目录下的 `plugins` 文件夹，重启 TrafficMonitor，在插件选项中填写管理密钥，然后在「显示设置」中勾选需要的显示项目。

## 目录结构

```
src/
  plugin.*          ITMPlugin / IPluginItem 实现，后台刷新与鼠标提示
  cpa_client.*      调用 CPA 管理接口，汇总每个账号的额度
  quota.*           账号列表、api-call、Codex/Claude 用量响应的解析（纯逻辑）
  json.*            极简 JSON 解析器
  http_client.*     WinHTTP 封装
  config.*          ini 读写与 DPAPI 加密
  options_dialog.*  选项对话框
  plugin.rc         对话框与版本资源
tests/              解析逻辑的单元测试（ctest）
third_party/TrafficMonitor/   上游插件接口头文件 PluginInterface.h（勿修改）及其许可证（Anti 996 License）
```

## 开发约定

- `IPluginItem` 的文本只在主线程（`DataRequired`）中更新；网络请求在后台线程进行，结果通过互斥量交接。
- 上游请求头（User-Agent 等）与 Management Center 的 `src/utils/quota/constants.ts` 保持一致。
- 不要重写 `ITMPlugin::GetAPIVersion`。升级插件接口时，从上游 `include/PluginInterface.h` 整体替换头文件。

## 许可证

[MIT](LICENSE)。`third_party/TrafficMonitor/PluginInterface.h` 来自 TrafficMonitor，遵循其 [Anti 996 License](third_party/TrafficMonitor/LICENSE)。
