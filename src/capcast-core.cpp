/*
 * capcast-core.cpp - CapCast 采集卡一键推流插件 核心实现
 *
 * 平台: Windows (显示扩展/音频枚举为 Win32 API, 跨平台可降级为仅 Qt 屏幕)
 * 依赖: libobs + obs-frontend-api + Qt6 Widgets
 */
#include "capcast-core.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.h>
#include <util/config-file.h> /* obs 31: 原 util/config.h 已合并到此 */

#include <QGuiApplication>
#include <QScreen>
#include <QApplication>

#ifdef _WIN32
#include <windows.h>
#include <winuser.h>
#include <mmdeviceapi.h>
#include <combaseapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <propidl.h>
#endif

/* ================= 内部状态 ================= */
namespace {

/* 单个音频源的监控类型快照(用于停止时恢复) */
struct SourceMonitorSnapshot {
	obs_source_t *source;
	enum obs_monitoring_type type;
};

struct OutputState {
	bool active = false;
	QString last_error;

	/* 音频路由状态: 全局监控设备 + 各源监控类型(均用于停止时恢复) */
	QString prev_monitor_name;
	QString prev_monitor_id;
	QVector<SourceMonitorSnapshot> source_snapshots;
};

OutputState g_state;

/* 与 OBS 自身一致的日志宏 */
#define CAPCAST_LOG(level, fmt, ...) \
	blog(level, "[CapCast] " fmt, ##__VA_ARGS__)

/* 配置段名 */
constexpr const char *CFG_SECTION = "CapCast";

/* 取 OBS 用户配置(全局, 随 profile 之外持久化) */
config_t *user_config()
{
	return obs_frontend_get_user_config();
}

/* UTF-8 宽字符辅助 (Windows) */
#ifdef _WIN32
QString wide_to_qstring(const wchar_t *w)
{
	return w ? QString::fromWCharArray(w) : QString();
}
#endif

} // namespace

/* ================= 显示器 ================= */

namespace capcast {

QVector<CapCastScreen> enum_screens()
{
	QVector<CapCastScreen> out;
	const auto screens = QGuiApplication::screens();
	for (int i = 0; i < screens.size(); ++i) {
		const QScreen *s = screens.at(i);
		CapCastScreen sc;
		sc.index = i;
		sc.name = s->name();
		sc.isPrimary = (s == QGuiApplication::primaryScreen());
		const QSize sz = s->size();
		int refresh = static_cast<int>(s->refreshRate());
		sc.geometry = QString("%1x%2@%3")
				      .arg(sz.width())
				      .arg(sz.height())
				      .arg(refresh);
		out.append(sc);
	}
	return out;
}

int find_screen_by_pattern(const QString &pattern)
{
	const QString p = pattern.trimmed().toLower();
	if (p.isEmpty())
		return -1;
	const auto screens = enum_screens();
	for (const auto &sc : screens) {
		/* 匹配设备名(如 \\.\DISPLAY2), 匹配友好名, 匹配几何信息 */
		if (sc.name.toLower().contains(p) ||
		    sc.geometry.toLower().contains(p))
			return sc.index;
	}
	return -1;
}

bool ensure_display_extend()
{
#ifdef _WIN32
	/* 强制"扩展"拓扑: 保留现有显示, 并点亮所有已连接副屏(含采集卡) */
	const LONG ret = SetDisplayConfig(
		0, nullptr, 0, nullptr,
		SDC_TOPOLOGY_EXTEND | SDC_APPLY | SDC_ALLOW_CHANGES);
	if (ret != ERROR_SUCCESS) {
		CAPCAST_LOG(LOG_WARNING, "SetDisplayConfig(EXTEND) failed: %ld", ret);
		return false;
	}
	/* 稍等系统应用显示拓扑 */
	Sleep(800);
	CAPCAST_LOG(LOG_INFO, "SetDisplayConfig(EXTEND) applied");
	return true;
#else
	CAPCAST_LOG(LOG_INFO, "ensure_display_extend: not supported on this platform");
	return false;
#endif
}

/* ================= 音频设备 ================= */

QVector<CapCastAudioDevice> enum_audio_devices()
{
	QVector<CapCastAudioDevice> out;
#ifdef _WIN32
	/* COM 初始化: 若 OBS 已初始化其它模式, 则忽略错误继续尝试 */
	HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (FAILED(hrInit) && hrInit != RPC_E_CHANGED_MODE)
		return out;

	IMMDeviceEnumerator *enumerator = nullptr;
	HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr,
				      CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
	if (SUCCEEDED(hr) && enumerator) {
		IMMDeviceCollection *coll = nullptr;
		hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE,
						   &coll);
		if (SUCCEEDED(hr) && coll) {
			UINT count = 0;
			coll->GetCount(&count);
			for (UINT i = 0; i < count; ++i) {
				IMMDevice *dev = nullptr;
				if (FAILED(coll->Item(i, &dev)) || !dev)
					continue;

				CapCastAudioDevice ad;

				LPWSTR wId = nullptr;
				if (SUCCEEDED(dev->GetId(&wId)) && wId) {
					ad.id = wide_to_qstring(wId);
					CoTaskMemFree(wId);
				}

				IPropertyStore *props = nullptr;
				if (SUCCEEDED(dev->OpenPropertyStore(
					    STGM_READ, &props)) &&
				    props) {
					PROPVARIANT varName;
					PropVariantInit(&varName);
					if (SUCCEEDED(props->GetValue(
						    PKEY_Device_FriendlyName,
						    &varName)) &&
					    varName.vt == VT_LPWSTR) {
						ad.name = wide_to_qstring(
							varName.pwszVal);
					}
					PropVariantClear(&varName);
					props->Release();
				}
				dev->Release();
				if (!ad.name.isEmpty() && !ad.id.isEmpty())
					out.append(ad);
			}
			coll->Release();
		}
		if (enumerator)
			enumerator->Release();
	}

	if (SUCCEEDED(hrInit))
		CoUninitialize();
#endif
	return out;
}

CapCastAudioDevice find_audio_device_by_pattern(const QString &pattern)
{
	const QString p = pattern.trimmed().toLower();
	if (p.isEmpty())
		return {};
	const auto devices = enum_audio_devices();
	for (const auto &d : devices) {
		if (d.name.toLower().contains(p))
			return d;
	}
	return {};
}

/* ================= 一键推流 ================= */

/* 把全部音频源设为"监听并输出", 使音频同时进 OBS 混音和全局监控设备(采集卡) */
static void set_monitor_all_sources(void *param, obs_source_t *source)
{
	auto *snapshots = static_cast<QVector<SourceMonitorSnapshot> *>(param);
	const uint32_t flags = obs_source_get_output_flags(source);
	/* 只路由有音频输出的源; 跳过辅助/占位源 */
	if ((flags & OBS_SOURCE_AUDIO) == 0)
		return;
	if (obs_source_get_type(source) == OBS_SOURCE_TYPE_SERVICE)
		return;

	SourceMonitorSnapshot snap;
	snap.source = source;
	snap.type = obs_source_get_monitoring_type(source);
	snapshots->append(snap);

	obs_source_set_monitoring_type(source,
				       OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT);
	CAPCAST_LOG(LOG_DEBUG, "monitor source: %s",
		    obs_source_get_name(source));
}

bool is_output_active()
{
	return g_state.active;
}

QString start_output(int screen_index, const QString &audio_device_id,
		     CapCastProjectorSource source)
{
	g_state.last_error.clear();

	/* 1. 校验目标屏幕存在 */
	const auto screens = enum_screens();
	if (screen_index < 0 || screen_index >= screens.size()) {
		g_state.last_error = QStringLiteral("采集卡副屏未找到(序号 %1 不存在)").arg(screen_index);
		return g_state.last_error;
	}

	/* 2. 打开全屏投影(节目/预览)到采集卡副屏 */
	/* obs 31: type 为字符串, "StudioProgram"=节目, nullptr=默认预览 */
	const char *projector_type =
		(source == CapCastProjectorSource::Program)
			? "StudioProgram"
			: nullptr;
	obs_frontend_open_projector(projector_type, screen_index, nullptr,
				    nullptr);
	CAPCAST_LOG(LOG_INFO, "projector opened on screen %d (%s)", screen_index,
		    qUtf8Printable(screens.at(screen_index).name));

	/* 3. 全部音频路由到采集卡: 把全局监控设备设为采集卡 + 全部源开监控 */
	/*    3.1 记录并保存当前监控设备, 停止时恢复 */
	const char *cur_name = nullptr;
	const char *cur_id = nullptr;
	obs_get_audio_monitoring_device(&cur_name, &cur_id);
	g_state.prev_monitor_name = cur_name ? QString::fromUtf8(cur_name) : QString();
	g_state.prev_monitor_id = cur_id ? QString::fromUtf8(cur_id) : QString();

	const QString device = audio_device_id.isEmpty()
				       ? QStringLiteral("default")
				       : audio_device_id;
	const QString dev_name =
		audio_device_id.isEmpty() ? QStringLiteral("采集卡") : QString();
	/*    3.2 设置全局监控设备为采集卡音频端点 */
	const bool ok = obs_set_audio_monitoring_device(
		qUtf8Printable(dev_name), qUtf8Printable(device));
	if (!ok) {
		CAPCAST_LOG(LOG_WARNING, "obs_set_audio_monitoring_device(%s) failed",
			    qUtf8Printable(device));
		g_state.last_error = QStringLiteral(
			"投影已打开, 但音频路由失败(设备 %1)").arg(device);
	} else {
		/*    3.3 全部音频源开启监控(记录原类型以便恢复) */
		g_state.source_snapshots.clear();
		obs_enum_sources(set_monitor_all_sources,
				 &g_state.source_snapshots);
		CAPCAST_LOG(LOG_INFO, "audio routed to %s (%d sources)",
			    qUtf8Printable(device),
			    (int)g_state.source_snapshots.size());
	}

	g_state.active = true;
	return {};
}

void stop_output()
{
	close_projector_windows();

	/* 恢复各音频源原监控类型 */
	for (const auto &snap : g_state.source_snapshots) {
		obs_source_set_monitoring_type(snap.source, snap.type);
	}
	g_state.source_snapshots.clear();

	/* 恢复原全局监控设备 */
	if (!g_state.prev_monitor_id.isEmpty() ||
	    !g_state.prev_monitor_name.isEmpty()) {
		obs_set_audio_monitoring_device(
			qUtf8Printable(g_state.prev_monitor_name),
			qUtf8Printable(g_state.prev_monitor_id));
		CAPCAST_LOG(LOG_INFO, "audio monitoring device restored");
	} else {
		obs_reset_audio_monitoring();
	}
	g_state.prev_monitor_name.clear();
	g_state.prev_monitor_id.clear();

	g_state.active = false;
}

void close_projector_windows()
{
	/* OBS 全屏投影是 OBSProjector(QDialog) 顶层窗口, 通过类名定位并关闭 */
	const QWidgetList widgets = QApplication::topLevelWidgets();
	for (QWidget *w : widgets) {
		if (w->inherits("OBSProjector")) {
			CAPCAST_LOG(LOG_INFO, "closing projector window");
			w->close();
		}
	}
}

/* ================= 配置持久化 ================= */

QString cfg_display_mode()
{
	return config_get_string(user_config(), CFG_SECTION, "DisplayMode");
}
void cfg_set_display_mode(const QString &m)
{
	config_set_string(user_config(), CFG_SECTION, "DisplayMode",
			  qUtf8Printable(m));
}
int cfg_display_index()
{
	return (int)config_get_int(user_config(), CFG_SECTION, "DisplayIndex");
}
void cfg_set_display_index(int idx)
{
	config_set_int(user_config(), CFG_SECTION, "DisplayIndex", idx);
}
QString cfg_display_pattern()
{
	return config_get_string(user_config(), CFG_SECTION, "DisplayPattern");
}
void cfg_set_display_pattern(const QString &p)
{
	config_set_string(user_config(), CFG_SECTION, "DisplayPattern",
			  qUtf8Printable(p));
}

QString cfg_audio_mode()
{
	return config_get_string(user_config(), CFG_SECTION, "AudioMode");
}
void cfg_set_audio_mode(const QString &m)
{
	config_set_string(user_config(), CFG_SECTION, "AudioMode",
			  qUtf8Printable(m));
}
QString cfg_audio_device_id()
{
	return config_get_string(user_config(), CFG_SECTION, "AudioDeviceId");
}
QString cfg_audio_device_name()
{
	return config_get_string(user_config(), CFG_SECTION, "AudioDeviceName");
}
void cfg_set_audio_device(const QString &name, const QString &id)
{
	config_set_string(user_config(), CFG_SECTION, "AudioDeviceName",
			  qUtf8Printable(name));
	config_set_string(user_config(), CFG_SECTION, "AudioDeviceId",
			  qUtf8Printable(id));
}
QString cfg_audio_pattern()
{
	return config_get_string(user_config(), CFG_SECTION, "AudioPattern");
}
void cfg_set_audio_pattern(const QString &p)
{
	config_set_string(user_config(), CFG_SECTION, "AudioPattern",
			  qUtf8Printable(p));
}

QString cfg_source()
{
	return config_get_string(user_config(), CFG_SECTION, "Source");
}
void cfg_set_source(const QString &s)
{
	config_set_string(user_config(), CFG_SECTION, "Source",
			  qUtf8Printable(s));
}

bool cfg_auto_extend()
{
	return config_get_bool(user_config(), CFG_SECTION, "AutoExtend");
}
void cfg_set_auto_extend(bool v)
{
	config_set_bool(user_config(), CFG_SECTION, "AutoExtend", v);
}
bool cfg_auto_start()
{
	return config_get_bool(user_config(), CFG_SECTION, "AutoStart");
}
void cfg_set_auto_start(bool v)
{
	config_set_bool(user_config(), CFG_SECTION, "AutoStart", v);
}

} // namespace capcast
