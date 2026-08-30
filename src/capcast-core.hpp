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

/* 屏幕信息 */
struct CapCastScreen {
	int index = -1;   /* Qt 屏幕序号, 与 OBS 全屏投影的 monitor 参数一致 */
	QString name;     /* 设备名, Windows 上形如 "\\.\DISPLAY2" */
	QString geometry; /* 形如 "1920x1080@60" */
	bool isPrimary = false;
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

namespace capcast {

/* ---------- 显示器 ---------- */

/* 枚举当前所有屏幕(与 OBS 投影编号一致: 0 起) */
QVector<CapCastScreen> enum_screens();

/* 自动选择"最像采集卡的副屏": 关键字优先, 兜底第一个非主屏; 无副屏返回 -1 */
int pick_default_screen();

/* 强制 Windows 显示模式为"扩展"(不动主屏、不复制), 用于把采集卡点亮成副屏 */
bool ensure_display_extend();

/* ---------- 音频设备 ---------- */

/* 枚举全部活动中的 WASAPI 渲染(输出)端点 */
QVector<CapCastAudioDevice> enum_audio_devices();

/* 自动选择"最像采集卡的音频端点": 关键字优先, 兜底第一个非音箱设备 */
CapCastAudioDevice pick_default_audio();

/* ---------- 一键推流 ---------- */

/* 当前是否处于"推流中"(投影已开 / 音频已路由) */
bool is_output_active();

/* 一键开始: 打开全屏投影 + 直接软路由全部音频到采集卡
 *   audio_device_id 为空时由 pick_default_audio() 自动选择
 *   返回错误描述, 成功返回空串
 */
QString start_output(int screen_index, const QString &audio_device_id,
		     CapCastProjectorSource source);

/* 一键停止: 关闭全屏投影窗口 + 移除音频路由过滤器 + 释放 WASAPI */
void stop_output();

/* 关闭所有 OBS 全屏投影窗口(尽力而为) */
void close_projector_windows();

/* ---------- 配置持久化 ---------- */

QString cfg_display_mode();                 /* "auto" | "manual" */
void cfg_set_display_mode(const QString &m);
int cfg_display_index();                    /* 手动指定屏幕序号 */
void cfg_set_display_index(int idx);

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

QString cfg_source();                       /* "program" | "preview" */
void cfg_set_source(const QString &s);

bool cfg_auto_start();                      /* 随 OBS 启动自动开始推流 */
void cfg_set_auto_start(bool v);

} // namespace capcast
