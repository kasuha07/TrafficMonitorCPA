# TrafficMonitorCPA

[TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor) 的轻量化插件（插件接口 API v8）。

- 纯 Win32 + C++20，不依赖 MFC/ATL
- 静态链接 CRT（`/MT`），DLL 只依赖 `KERNEL32.dll`，无需安装 VC++ 运行库
- Release 使用 `/O1 /GL /LTCG /OPT:REF,ICF` 以减小体积

## 环境要求

- Visual Studio 生成工具 2026（MSVC v145 / 14.50），Windows SDK 10.0.26100
- CMake ≥ 3.25（需支持 `Visual Studio 18 2026` 生成器）

## 构建

```bash
cmake --preset x64
```

```bash
cmake --build --preset x64-release
```

32 位版本使用 `x86` / `x86-release` 预设。产物位于 `build/<arch>/<Config>/TrafficMonitorCPA.dll`。

## 安装

将与 TrafficMonitor 位数一致的 `TrafficMonitorCPA.dll` 复制到 TrafficMonitor 目录下的 `plugins` 文件夹，重启 TrafficMonitor，然后在「插件管理」中启用该插件，并在「显示设置」中勾选它的显示项目。

## 目录结构

```
src/                          插件源码
  plugin.h / plugin.cpp       ITMPlugin / IPluginItem 实现，导出 TMPluginGetInstance
  version.h.in / version.rc   版本信息（来自 CMake project VERSION）
third_party/TrafficMonitor/   上游插件接口头文件 PluginInterface.h（勿修改）
```

## 开发约定

- `IPluginItem::GetItemValueText` 等函数会被频繁调用，只返回缓存值；数据采集放在 `ITMPlugin::DataRequired` 中。
- 不要重写 `ITMPlugin::GetAPIVersion`。
- 升级插件接口时，从上游 `include/PluginInterface.h` 整体替换 `third_party/TrafficMonitor/PluginInterface.h`。
