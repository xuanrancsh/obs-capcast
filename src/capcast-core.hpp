/*
 * capcast-core.hpp - CapCast 采集卡一键推流插件 核心逻辑
 *
 * 职责：
 *   1. 采集卡副屏识别（Qt screens 枚举 + 智能选择）
 *   2. 采集卡音频端点识别（WASAPI 枚举 + 智能选择）
 *   3. 一键推流：全屏投影(obs_frontend_open_projector) +
 *                直接软路由全部音频到采集卡(WASAPI 直出, 绕开 OBS 混音器)
 *   4. 配置持久化（OBS user config, 段名 "CapCast", 每次写入即保存）
 */
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QSize>

/* 屏幕信息 */
struct CapCastScreen {
	int index = -1;   /* Qt 屏幕序号, 与 OBS 全屏投影的 monitor 参数一致 */
	QString name;     /* 设备名, Windows 上形如 "\\.\DISPLAY2" */
	QString geometry; /* 物理分辨率, 形如 "3840x2160@60" */
	bool isPrimary = false;

	/* 分辨率细节:
	 *   physical = 显示器真实像素(如 4K 屏 = 3840x2160)
	 *   logical  = Qt 报告的像素, 已被 Windows 系统缩放除过(4K@300% -> 1280x720)
	 *   scale    = 系统缩放倍数(1.0=100%, 3.0=300%)
	 * 注意: 只有 physical 是显示器的真实能力; logical 仅供排查用,
	 *       插件投屏不受它影响(投影窗口是全屏放到该物理显示器上的)。 */
	QSize physical; /* 物理像素 */
	QSize logical;  /* 逻辑像素(Qt 报告值) */
	double scale = 1.0;
};

/* WASAPI 输出端点(渲染设备)信息 */
struct CapCastAudioDevice {
	QString name; /* 友好名, 如 "Elgato HD60 X" */
	QString id;   /* 设备 ID, 如 "{0.0.0.00000000}.{guid}" */
};

/* 推流源类型 */
enum class CapCastProjectorSource {
	Program, /* 节目(PGM) */
	Preview, /* 预览(PVW) */
};

/* 设备种类(掉线提醒用) */
enum class CapCastDeviceKind {
	Display, /* 采集卡副屏 */
	Audio,   /* 采集卡音频端点 */
};

/* 上次绑定的设备, 本次能否再找到。
 *
 * 为什么要区分"名对上了/ID 变了":
 *   Windows 的 WASAPI 设备 ID 形如 "{0.0.0.00000000}.{GUID}", USB 采集卡
 *   换个接口或重新枚举后 GUID 会变, 但友好名(如 "Elgato HD60 X")通常不变。
 *   同理, 显示器设备名("\\.\DISPLAY2")稳定, 但它在 Qt 列表里的**序号**
 *   会随插拔/开关机变化 —— 用序号做持久化会悄悄指向另一块屏。
 *   因此一律"名称优先", ID 只作辅助校验。 */
enum class CapCastDeviceMatch {
	Exact,   /* 精确命中 */
	Renamed, /* 名称命中但 ID 已变(已把新 ID 回写进配置) */
	Changed, /* 自动模式下选中了与上次不同的设备(要提示, 不能静默) */
	Missing, /* 列表里再也找不到这个设备 */
	Unbound, /* 从未绑定过(首次使用, 自动模式第一次选) */
};

/* 屏幕绑定解析结果 */
struct CapCastScreenBinding {
	int index = -1; /* 解析出的 Qt 屏幕序号, <0 表示没找到 */
	CapCastDeviceMatch match = CapCastDeviceMatch::Unbound;
	QString name; /* 绑定的设备名, 如 "\\.\DISPLAY2" */
};

/* 音频绑定解析结果 */
struct CapCastAudioBinding {
	QString id;
	QString name;
	CapCastDeviceMatch match = CapCastDeviceMatch::Unbound;
};

namespace capcast {

/* ---------- 显示器 ---------- */

/* 枚举当前所有屏幕(与 OBS 投影编号一致: 0 起) */
QVector<CapCastScreen> enum_screens();

/* 自动选择"最像采集卡的副屏": 关键字优先, 兜底第一个非主屏; 无副屏返回 -1 */
int pick_default_screen();

/* 解析"上次用的那块屏"是否还在。
 * 名称优先匹配, index 以本次枚举结果为准(序号会漂移, 不能直接用旧值)。
 * 未绑定过则回退到自动选择, 并把结果固化下来。 */
CapCastScreenBinding resolve_screen_binding();

/* 固化屏幕绑定(用户手动选择, 或自动模式首次选定后落盘) */
void bind_screen(const QString &name);

/* 强制 Windows 显示模式为"扩展"(不动主屏、不复制), 用于把采集卡点亮成副屏 */
bool ensure_display_extend();

/* ---------- 音频设备 ---------- */

/* 枚举全部活动中的 WASAPI 渲染(输出)端点 */
QVector<CapCastAudioDevice> enum_audio_devices();

/* 自动选择"最像采集卡的音频端点": 关键字优先, 兜底第一个非音箱设备 */
CapCastAudioDevice pick_default_audio();

/* 解析"上次用的那个音频端点"是否还在。
 * 先按 ID 精确匹配; ID 变了再按友好名匹配(命中则回写新 ID)。
 * 两者都失败 -> Missing, 调用方必须提示用户而不是改用别的设备。 */
CapCastAudioBinding resolve_audio_binding();

/* 固化音频绑定 */
void bind_audio(const QString &id, const QString &name);

/* ---------- 设备掉线监控 ----------
 * 运行期间监听设备消失: 音频走 WASAPI 设备通知 + 写入失败计数兜底,
 * 显示走 QGuiApplication::screenRemoved。
 * 掉线时只做"停止该项 + 提醒", 绝不自动换设备。 */

/* 启动/停止设备监控(插件加载/卸载时调用) */
void device_monitor_start();
void device_monitor_stop();

/* 该项是否因设备掉线而处于"停摆"状态(供面板显示真实状态) */
bool is_device_lost(CapCastDeviceKind kind);

/* 掉线后设备又回来了: 返回 true 表示可以重新应用 */
bool is_device_returned(CapCastDeviceKind kind);

/* 抑制提醒(OBS 退出流程中不要弹窗挡住退出) */
void set_lost_notifications_enabled(bool on);

/* ---------- 投屏 / 音频映射(各自独立控制) ----------
 * 投屏与音频映射是两项互不依赖的功能, 可分别开启/关闭。
 * 两者共用同一套目标设备配置, 但运行状态各自独立维护。 */

/* 投屏(全屏投影到采集卡副屏)是否正在运行 */
bool is_projector_active();

/* 音频映射(主混音 -> 采集卡端点)是否正在运行 */
bool is_audio_active();

/* 兼容/便捷判断: 任一项在跑即视为"推流中" */
bool is_output_active();

/* 只开投屏。返回错误描述, 成功返回空串。
 * screen_index < 0 时由 pick_default_screen() 自动选择。
 * 只会记录并关闭"本插件自己打开"的那个投影窗口。 */
QString start_projector(int screen_index, CapCastProjectorSource source);

/* 只关投屏: 仅关闭本插件打开的那个投影窗口, 不影响用户手动打开的投影。 */
void stop_projector();

/* 只开音频映射(订阅 OBS 主混音 -> 采集卡端点)。返回错误描述, 成功返回空串。
 * audio_device_id 为空时由 pick_default_audio() 自动选择。 */
QString start_audio_mapping(const QString &audio_device_id);

/* 只关音频映射(取消订阅 + 释放 WASAPI) */
void stop_audio_mapping();

/* 批量开始: 按 EnableProjector / EnableAudio 两个勾选分别执行。
 * 两项都没勾选 -> 不做任何操作, 返回空串。
 * 某项失败 -> 成功项保持运行, 返回汇总错误描述(便于界面如实提示)。 */
QString start_enabled();

/* 批量停止: 停掉当前正在运行的全部(投屏 + 音频), 与勾选状态无关。 */
void stop_all();

/* ---------- 配置持久化 ---------- */

QString cfg_display_mode();                 /* "auto" | "manual" */
void cfg_set_display_mode(const QString &m);
int cfg_display_index(); /* 手动指定屏幕序号(旧配置残留, 仅作兜底) */
void cfg_set_display_index(int idx);
QString cfg_display_name(); /* 上次绑定的屏幕设备名, 如 "\\.\DISPLAY2" */
void cfg_set_display_name(const QString &n);

QString cfg_audio_mode();                   /* "auto" | "manual" */
void cfg_set_audio_mode(const QString &m);
QString cfg_audio_device_id();              /* 手动指定音频设备 ID */
QString cfg_audio_device_name();
void cfg_set_audio_device(const QString &name, const QString &id);

/* 输出到采集卡的音量(百分比): 100 = 原始主混音音量, 50 = 一半, 默认 50 */
double cfg_audio_volume();
void cfg_set_audio_volume(double percent);

/* 运行时调整输出音量: 立即生效(无需重启路由), 不落盘
 * (拖动滑块会高频调用, 持久化请用 cfg_set_audio_volume)。
 * 只影响送到采集卡的声音, 不改 OBS 混音器里的任何音量。 */
void set_output_volume(double percent);

/* 订阅哪一条音轨(1-6, 对应 OBS 混音器里的音轨 1-6, 默认 1)。
 * 只有勾选了该音轨的源才会出现在这一路混音里。 */
int cfg_audio_track();
void cfg_set_audio_track(int track);

/* 运行中切换订阅音轨: 立即生效(先按原 mix 索引取消订阅, 再按新索引订阅)。
 * 未运行时只记录, 下次开始时生效。 */
void apply_audio_track(int track);

/* 只停止音频路由, 不触碰任何 Qt 窗口。
 * 供 obs_module_unload 调用: 卸载阶段 OBS 正在销毁主窗口与投影窗口,
 * 此时遍历/关闭 QWidget 会把事件派发到已析构的对象上导致崩溃。
 * 投影窗口交由 OBS 自己销毁。 */
void stop_audio_only();

QString cfg_source();                       /* "program" | "preview" */
void cfg_set_source(const QString &s);

/* 投屏功能是否勾选(默认 true)。
 * 既是"一键推流"的执行清单, 也是 OBS 自动启动时的执行清单。 */
bool cfg_enable_projector();
void cfg_set_enable_projector(bool v);

/* 音频映射功能是否勾选(默认 true)。语义同上。 */
bool cfg_enable_audio();
void cfg_set_enable_audio(bool v);

/* 总闸: OBS 启动时是否自动开始推流(默认 false)。
 * 关闭时插件在 OBS 启动后完全不动作;
 * 开启时按上面两个勾选分别启动(两项都没勾则不操作)。 */
bool cfg_auto_start();
void cfg_set_auto_start(bool v);

/* 找不到采集卡副屏时, 是否自动把显示拓扑设为"扩展"(默认 true) */
bool cfg_auto_extend();
void cfg_set_auto_extend(bool v);

} // namespace capcast
