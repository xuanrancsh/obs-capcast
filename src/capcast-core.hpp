/*
 * capcast-core.hpp - CapCast 采集卡一键推流插件 核心逻辑
 *
 * 职责：
 *   1. 采集卡副屏识别（Qt screens 枚举 + 名称匹配）+ 可选自动扩展显示（SetDisplayConfig）
 *   2. 采集卡音频端点识别（WASAPI 枚举 + 名称匹配）
 *   3. 一键推流：全屏投影(obs_frontend_open_projector) + 全部音频路由到采集卡(audio_monitor)
 *   4. 配置持久化（OBS user config, 段名 "CapCast"）
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

/* 按名称子串匹配采集卡屏幕; 返回 Qt 屏幕序号, 找不到返回 -1 */
int find_screen_by_pattern(const QString &pattern);

/* 强制 Windows 显示模式为"扩展"(不动主屏、不复制), 用于把采集卡点亮成副屏 */
bool ensure_display_extend();

/* ---------- 音频设备 ---------- */

/* 枚举全部活动中的 WASAPI 渲染(输出)端点 */
QVector<CapCastAudioDevice> enum_audio_devices();

/* 按名称子串匹配采集卡音频端点; 返回匹配设备, 找不到返回空 */
CapCastAudioDevice find_audio_device_by_pattern(const QString &pattern);

/* ---------- 一键推流 ---------- */

/* 当前是否处于"推流中"(投影已开 / 音频已路由) */
bool is_output_active();

/* 一键开始: 打开全屏投影 + 全部音频路由到采集卡 */
/* 返回错误描述, 成功返回空串 */
QString start_output(int screen_index, const QString &audio_device_id,
		     CapCastProjectorSource source);

/* 一键停止: 关闭全屏投影窗口 + 销毁音频路由 */
void stop_output();

/* 关闭所有 OBS 全屏投影窗口(尽力而为) */
void close_projector_windows();

/* ---------- 配置持久化 ---------- */

QString cfg_display_mode();                 /* "auto" | "manual" */
void cfg_set_display_mode(const QString &m);
int cfg_display_index();                    /* 手动指定屏幕序号 */
void cfg_set_display_index(int idx);
QString cfg_display_pattern();              /* 自动匹配名称子串 */
void cfg_set_display_pattern(const QString &p);

QString cfg_audio_mode();                   /* "auto" | "manual" */
void cfg_set_audio_mode(const QString &m);
QString cfg_audio_device_id();              /* 手动指定音频设备 ID */
QString cfg_audio_device_name();
void cfg_set_audio_device(const QString &name, const QString &id);
QString cfg_audio_pattern();
void cfg_set_audio_pattern(const QString &p);

QString cfg_source();                       /* "program" | "preview" */
void cfg_set_source(const QString &s);

bool cfg_auto_extend();                     /* 找不到副屏时自动扩展显示 */
void cfg_set_auto_extend(bool v);
bool cfg_auto_start();                      /* OBS 启动时自动开始 */
void cfg_set_auto_start(bool v);

} // namespace capcast
