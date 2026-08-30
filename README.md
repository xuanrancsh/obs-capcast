# 采集卡一键推流（CapCast）

**一键把 OBS 画面 + 全部音频推送到采集卡（副屏）**，用于双机推流 / 采集卡环出场景：
采集卡被 Windows 识别为副屏 → OBS 全屏投影投到副屏 → 全部音频自动路由到采集卡音频端点 →
第二台电脑在采集卡输入端看到同步的音画。全程零手工配置，发给别人装上就能用。

> 基于 OBS Studio 官方插件模板（obs-plugintemplate）开发，音频路由思路借鉴了
> DistroAV 与 exeldro/obs-audio-monitor（详见文末「致谢」）。

## 功能

- **一键开始 / 一键停止**（工具菜单 + 全局热键）
- 自动**全屏投影**到采集卡副屏（节目 PGM / 预览 PVW 可选）
- **独立音频路由**：订阅 OBS 主混音成品直出采集卡，**完全不改动 OBS 混音器**的音量 / 静音 / 监听设置，也不使用 `audio_monitor` API。混音、重采样、声道映射全部由 OBS 音频引擎完成，因此混音器里的滤镜效果（降噪 / 增益 / VST 等）也会一并带过去
- **输出音量**控制：0–200%，默认 50%，推流中拖动即时生效
- **订阅音轨选择**：1–6，可切换只推某条音轨
- 采集卡副屏 / 音频端点**自动识别**（按名称子串匹配，如 `capture` / `elgato`），也可手动指定
- 找不到副屏时，可选**自动扩展显示**（`SetDisplayConfig`，只扩展不复制、不动主屏）
- **OBS 启动时自动开始**（可配置）

## 使用

1. 安装后，打开 OBS → 顶部菜单 **工具 → 采集卡一键推流**
2. 面板里确认「目标屏幕」和「音频设备」识别正确（可选手动指定）
3. 点 **一键开始**：全屏投影打开、全部音频进采集卡
4. 第二台电脑在采集卡输入端看到画面 + 声音，无需任何额外设置

> 前置条件：采集卡已作为副屏接入（Windows 显示设置里已「扩展」）。若没接好，勾选
> 「找不到采集卡副屏时自动扩展显示」后点开始，插件会帮你点亮。

## 兼容性

- 构建基于 **OBS Studio 31.1.1**，已在 **OBS 32.0.4** 验证可用。
- 按 OBS 插件惯例，面向较旧 OBS 构建的插件可向前兼容更高版本（同一 libobs API 周期内一般无需重新编译）。
- 平台：Windows（采集卡直连走 WASAPI）。

## 构建 / 安装包

本仓库自包含 obs-plugintemplate 构建脚手架（`cmake/`、`.github/`）。推送到 GitHub 后
Actions 自动构建 Windows 安装包；打语义化版本标签（如 `1.0.0`）会自动生成 Release 与安装包。
Release 中的安装包名为 `CapCast-<版本>-windows-x64-Installer.exe`。

## 目录结构

```
capcast/
├── CMakeLists.txt          # 构建配置(模板体系)
├── buildspec.json          # 插件元数据 + OBS/依赖版本
├── .github/workflows/      # CI 构建与 Release
├── src/
│   ├── plugin-main.cpp     # 模块入口: 菜单/事件/热键/卸载清理
│   ├── capcast-core.cpp    # 核心: 副屏识别/扩展、音频识别、投影+音频路由、配置
│   └── capcast-settings.cpp# 设置面板(音量/音轨/屏幕/设备)
└── data/locale/            # en-US / zh-CN
```

## 致谢

本项目在开发与实现上参考 / 复用了以下开源项目，特此致谢：

- **OBS Studio** — 插件运行平台与 libobs API：`https://github.com/obsproject/obs-studio`
- **obs-plugintemplate** — 本插件的构建脚手架与 CI 模板：`https://github.com/obsproject/obs-plugintemplate`
- **DistroAV**（原 obs-ndi）— NDI 音频 / 视频集成的架构参考：`https://github.com/DistroAV/DistroAV`
- **exeldro/obs-audio-monitor** — 音频设备路由的思路参考：`https://github.com/exeldro/obs-audio-monitor`

## 开源与贡献（GitHub 礼仪）

- **许可证**：本项目基于 **GPL-2.0** 发布（与 OBS Studio 一致）。依 GPL 要求，分发二进制时须提供对应源码——本公开仓库即源码所在地，欢迎查看、二次分发与修改。
- **源码公开**：所有构建均来自本仓库的 CI，未做闭源修改。
- **问题反馈**：请在仓库 Issues 区提交，尽量附上 OBS 日志（`帮助 → 日志文件 → 显示日志目录`）与复现步骤。
- **贡献**：欢迎 PR；请保持代码风格一致，较大改动建议先开 Issue 讨论。
- **署名**：基于本插件二次开发并分发时，请保留上述「致谢」与许可证信息。
