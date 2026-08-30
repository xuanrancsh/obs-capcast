# CapCast 采集卡一键推流插件

**一键把 OBS 画面+全部音频推给采集卡（副屏）**，用于双机推流/采集卡环出场景：
采集卡被 Windows 识别为副屏 → OBS 全屏投影投到副屏 → 全部音频自动路由到采集卡音频端点 →
采集卡把音画同步送给第二台电脑。全程零手工配置，发给别人装上就能用。

## 功能

- **一键开始 / 一键停止**（工具菜单 + 全局热键）
- 自动打开**全屏投影**到采集卡副屏（节目 PGM / 预览 PVW 可选）
- 把**全部音频源**路由到采集卡音频端点（基于 OBS `audio_monitor`，参考 exeldro/obs-audio-monitor）
- 采集卡副屏/音频端点**自动识别**（按名称子串匹配，如 `capture`/`elgato`），也可手动指定
- 找不到副屏时，可选**自动扩展显示**（`SetDisplayConfig`，只扩展不复制、不动主屏）
- **OBS 启动时自动开始**（可配置）
- 设置面板实时显示识别结果与运行状态

## 使用

1. 编译安装后，打开 OBS → 顶部菜单 **工具 → CapCast 采集卡一键推流**
2. 面板里确认"目标屏幕"和"音频设备"识别正确（可选手动指定）
3. 点 **一键开始**：全屏投影打开、全部音频进采集卡
4. 第二台电脑在采集卡输入端看到画面+声音，无需任何额外设置

> 前置条件：采集卡已作为副屏接入（Windows 显示设置里已"扩展"）。若没接好，勾选
> "找不到采集卡副屏时自动扩展显示" 后点开始，插件会帮你点亮。

## 构建（推 GitHub 自动出安装包）

本仓库**自包含** obs-plugintemplate 构建脚手架（`cmake/`、`.github/`），
推送到 GitHub 后 Actions 自动构建 Windows 安装包（zip + exe）：

```bash
git init && git add -A && git commit -m "CapCast 1.0.0"
git remote add origin https://github.com/<你的账号>/obs-capcast.git
git push -u origin main
```

构建流程：push 触发 `.github/workflows/push.yaml` → `build-project.yaml`
（Windows x64，obs-deps/Qt 由官方 CI 自动下载）→ 产物 zip/exe 出现在
**Actions 运行记录的 Artifacts** 里；打版本标签（如 `1.0.0`）还会自动创建 Release。

也可手动触发：仓库 Actions 页 → Push to master → Run workflow。
本地构建（需要 VS2022）：`.github/scripts/Build-Windows.ps1 -Target x64`。

安装：把 zip 里的 `obs-plugins/capcast` 拷到 OBS 的 `obs-plugins/`，
`data/locale` 拷到 `data/obs-plugins/capcast/`（标准 OBS 插件布局，与 DistroAV 相同）。

## 目录结构

```
capcast/
├── CMakeLists.txt          # 构建配置(模板体系)
├── buildspec.json          # 插件元数据 + OBS/依赖版本
├── .github/workflows/      # CI 构建
├── src/
│   ├── plugin-main.cpp     # 模块入口: 菜单/事件/热键
│   ├── capcast-core.cpp    # 核心: 副屏识别/扩展、音频识别、投影+音频路由、配置
│   └── capcast-settings.cpp# 设置面板
└── data/locale/            # en-US / zh-CN
```

## 已知限制 / 待办

- 停止时关闭全屏投影采用"按类名关闭 OBSProjector 窗口"的尽力而为方式
- 音频路由使用 OBS 公共 `audio_monitor` API（运行时生效，无需重启 OBS）
- 后续可加：音频静音/丢流告警（参考 AudioCaptureAlert）、健康状态 Dock、多采集卡、副屏分辨率自动对齐画布
