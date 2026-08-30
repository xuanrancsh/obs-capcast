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
#include <media-io/audio-resampler.h>

#include <QGuiApplication>
#include <QScreen>
#include <QApplication>
#include <QWidget>

#ifdef _WIN32
#include <windows.h>
#include <winuser.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <combaseapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <propidl.h>
#include <mutex>
#endif

/* ================= 内部状态 ================= */
namespace {

/* 已挂上路由过滤器的场景/源, 停止时移除 */
struct RouterAttachment {
	obs_source_t *host;
	obs_source_t *filter;
};

struct OutputState {
	bool active = false;
	QString last_error;

	/* 直接音频路由: WASAPI 输出 + 挂在各场景上的过滤器 */
	QVector<RouterAttachment> router_attachments;
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

/* ================= 直接音频路由(WASAPI 直出, 绕开 OBS 混音器) =================
 * 思路(参考 exeldro/obs-audio-monitor):
 *   - 注册一个隐藏音频过滤器 capcast_audio_router
 *   - 开始推流时: 初始化 WASAPI 输出到采集卡设备, 并把过滤器挂到所有场景上
 *   - 场景的混合音频 → filter_audio 回调 → 重采样 → WASAPI 直出到采集卡
 *   - 完全不改动 OBS 混音器/监控设备设置
 */
#ifdef _WIN32

/* WASAPI 渲染端点(shared mode, 跟随设备混音格式) */
struct WasapiOutput {
	IMMDevice *device = nullptr;
	IAudioClient *client = nullptr;
	IAudioRenderClient *render = nullptr;
	UINT32 buffer_frames = 0;
	UINT32 channels = 2;
	UINT32 sample_rate = 48000;
	bool active = false;

	/* 重采样: OBS float-planar 48k → 设备 float-interleaved */
	audio_resampler_t *resampler = nullptr;
	uint8_t *resample_buf[MAX_AV_PLANES] = {};
	uint32_t resample_frames = 0;
};

static WasapiOutput g_wasapi;

static void wasapi_shutdown()
{
	if (g_wasapi.resampler) {
		audio_resampler_destroy(g_wasapi.resampler);
		g_wasapi.resampler = nullptr;
	}
	if (g_wasapi.render)
		g_wasapi.render->Release();
	g_wasapi.render = nullptr;
	if (g_wasapi.client)
		g_wasapi.client->Release();
	g_wasapi.client = nullptr;
	if (g_wasapi.device)
		g_wasapi.device->Release();
	g_wasapi.device = nullptr;
	g_wasapi.active = false;
}

static bool wasapi_start(const char *device_id)
{
	HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool need_uninit = SUCCEEDED(hr) || hr == S_FALSE;
	(void)need_uninit;

	IMMDeviceEnumerator *enumerator = nullptr;
	/* CLSID_MMDeviceEnumerator 本地定义, 避免 uuid.lib 符号问题 */
	static const GUID clsid_mmdevice_enumerator = {
		0xBCDE0395, 0xE52F, 0x467C,
		{0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};

	hr = CoCreateInstance(clsid_mmdevice_enumerator, nullptr, CLSCTX_ALL,
			      IID_PPV_ARGS(&enumerator));
	if (FAILED(hr) || !enumerator) {
		CAPCAST_LOG(LOG_ERROR, "wasapi: no enumerator");
		return false;
	}

	if (!device_id || strcmp(device_id, "default") == 0) {
		hr = enumerator->GetDefaultAudioEndpoint(
			eRender, eConsole, &g_wasapi.device);
	} else {
		wchar_t wide_id[512];
		MultiByteToWideChar(CP_UTF8, 0, device_id, -1, wide_id, 512);
		hr = enumerator->GetDevice(wide_id, &g_wasapi.device);
	}
	enumerator->Release();
	if (FAILED(hr) || !g_wasapi.device) {
		CAPCAST_LOG(LOG_ERROR, "wasapi: device not found (%s)",
			    device_id ? device_id : "(default)");
		return false;
	}

	hr = g_wasapi.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
				       nullptr, (void **)&g_wasapi.client);
	if (FAILED(hr) || !g_wasapi.client) {
		CAPCAST_LOG(LOG_ERROR, "wasapi: activate client failed");
		wasapi_shutdown();
		return false;
	}

	WAVEFORMATEX *wfex = nullptr;
	hr = g_wasapi.client->GetMixFormat(&wfex);
	if (FAILED(hr) || !wfex) {
		wasapi_shutdown();
		return false;
	}
	g_wasapi.sample_rate = wfex->nSamplesPerSec;
	g_wasapi.channels = wfex->nChannels;

	hr = g_wasapi.client->Initialize(
		AUDCLNT_SHAREMODE_SHARED, 0, 10000000 /* 100ms */, 0, wfex,
		nullptr);
	CoTaskMemFree(wfex);
	if (FAILED(hr)) {
		CAPCAST_LOG(LOG_ERROR, "wasapi: initialize failed (0x%08lX)", hr);
		wasapi_shutdown();
		return false;
	}

	hr = g_wasapi.client->GetBufferSize(&g_wasapi.buffer_frames);
	if (FAILED(hr)) {
		wasapi_shutdown();
		return false;
	}
	hr = g_wasapi.client->GetService(IID_PPV_ARGS(&g_wasapi.render));
	if (FAILED(hr) || !g_wasapi.render) {
		wasapi_shutdown();
		return false;
	}

	/* 重采样器: OBS 输出(float planar, 48k, 实际布局) → 设备(float interleaved) */
	const struct audio_output_info *info =
		audio_output_get_info(obs_get_audio());
	/* 设备声道数 → obs speaker 布局 */
	const auto layout_from_channels = [](UINT32 n) {
		switch (n) {
		case 1:
			return SPEAKERS_MONO;
		case 3:
			return SPEAKERS_2POINT1;
		case 4:
			return SPEAKERS_4POINT0;
		case 6:
			return SPEAKERS_5POINT1;
		case 8:
			return SPEAKERS_7POINT1;
		default:
			return SPEAKERS_STEREO;
		}
	};
	struct resample_info from{};
	struct resample_info to{};
	from.samples_per_sec = info ? info->samples_per_sec : 48000;
	from.speakers = info ? info->speakers : SPEAKERS_STEREO;
	from.format = AUDIO_FORMAT_FLOAT_PLANAR;
	to.samples_per_sec = g_wasapi.sample_rate;
	to.speakers = layout_from_channels(g_wasapi.channels);
	to.format = AUDIO_FORMAT_FLOAT;
	g_wasapi.resampler = audio_resampler_create(&to, &from);
	if (!g_wasapi.resampler) {
		CAPCAST_LOG(LOG_ERROR, "wasapi: resampler create failed");
		wasapi_shutdown();
		return false;
	}

	hr = g_wasapi.client->Start();
	if (FAILED(hr)) {
		wasapi_shutdown();
		return false;
	}
	g_wasapi.active = true;
	CAPCAST_LOG(LOG_INFO, "wasapi output started (%uHz %uch)",
		    g_wasapi.sample_rate, g_wasapi.channels);
	return true;
}

/* 把 resample 后的 planar 缓冲转成设备期望的 interleaved float 并写入 */
static void wasapi_write_audio(const uint8_t *const data[MAX_AV_PLANES],
			       uint32_t frames)
{
	if (!g_wasapi.active || !g_wasapi.render || frames == 0)
		return;

	/* 重采样到设备格式 */
	uint32_t out_frames = 0;
	uint64_t ts_offset = 0;
	if (!audio_resampler_resample(g_wasapi.resampler,
				      g_wasapi.resample_buf, &out_frames,
				      &ts_offset, data, frames)) {
		return;
	}
	if (out_frames == 0)
		return;
	g_wasapi.resample_frames = out_frames;

	/* 逐块写入设备环形缓冲 */
	float *out = (float *)g_wasapi.resample_buf[0];
	uint32_t remaining = out_frames;
	while (remaining > 0) {
		UINT32 pad = 0;
		if (FAILED(g_wasapi.client->GetCurrentPadding(&pad)))
			return;
		UINT32 avail = g_wasapi.buffer_frames - pad;
		if (avail == 0) {
			/* 环形缓冲满: 丢弃剩余(不能阻塞音频线程) */
			break;
		}
		UINT32 chunk = remaining < avail ? remaining : avail;
		float *dst = nullptr;
		HRESULT hr = g_wasapi.render->GetBuffer(chunk, (BYTE **)&dst);
		if (FAILED(hr)) {
			if (hr == AUDCLNT_E_BUFFER_TOO_LARGE ||
			    hr == AUDCLNT_E_BUFFER_ERROR) {
				g_wasapi.render->ReleaseBuffer(chunk, 0);
				continue;
			}
			return;
		}
		memcpy(dst, out, chunk * g_wasapi.channels * sizeof(float));
		out += chunk * g_wasapi.channels;
		remaining -= chunk;
		g_wasapi.render->ReleaseBuffer(chunk, 0);
	}
}

#endif /* _WIN32 */

/* 音频路由过滤器源类型: 挂到场景上, 把场景混合音频转发给 WASAPI 直出 */
static const char *router_get_name(void *)
{
	return "CapCast Audio Router";
}

static void *router_create(obs_data_t *, obs_source_t *)
{
	return bzalloc(sizeof(uint8_t));
}

static void router_destroy(void *data)
{
	bfree(data);
}

static struct obs_audio_data *router_filter_audio(void *, struct obs_audio_data *audio)
{
#ifdef _WIN32
	if (g_wasapi.active && audio && audio->frames > 0) {
		wasapi_write_audio((const uint8_t *const *)audio->data,
				   audio->frames);
	}
#endif
	return audio; /* 不改动原音频, 正常继续走 OBS 混音 */
}

/* obs_source_info 用零初始化+逐字段赋值(C++17 不支持指定初始化器) */
static struct obs_source_info capcast_audio_router = {};
void capcast_router_register()
{
	capcast_audio_router.id = "capcast_audio_router";
	capcast_audio_router.type = OBS_SOURCE_TYPE_FILTER;
	capcast_audio_router.output_flags =
		OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
	capcast_audio_router.get_name = router_get_name;
	capcast_audio_router.create = router_create;
	capcast_audio_router.destroy = router_destroy;
	capcast_audio_router.filter_audio = router_filter_audio;
	obs_register_source(&capcast_audio_router);
}

/* 开始直接音频路由: 初始化 WASAPI + 给所有场景挂过滤器 */
static QString start_audio_routing(const QString &device_id)
{
#ifdef _WIN32
	const QString dev_id = device_id.isEmpty() ? QStringLiteral("default")
						   : device_id;
	if (!wasapi_start(qUtf8Printable(dev_id)))
		return QStringLiteral("音频设备打开失败(%1)").arg(dev_id);

	/* 把过滤器挂到所有场景上 */
	struct RouterCallbackData {
		QVector<RouterAttachment> *list;
		obs_source_t *router;
	} cb;
	cb.list = &g_state.router_attachments;
	cb.router = nullptr;

	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			auto *d = static_cast<RouterCallbackData *>(param);
			if (obs_source_get_type(source) != OBS_SOURCE_TYPE_SCENE)
				return true;
			/* 每个场景创建一个独立过滤器实例 */
			obs_source_t *filter =
				obs_source_create_private("capcast_audio_router",
							  "capcast-router", nullptr);
			if (!filter)
				return true;
			obs_source_filter_add(source, filter);
			RouterAttachment att;
			att.host = source;
			att.filter = filter;
			d->list->append(att);
			CAPCAST_LOG(LOG_DEBUG, "router attached to scene %s",
				    obs_source_get_name(source));
			return true;
		},
		&cb);

	if (g_state.router_attachments.isEmpty()) {
		CAPCAST_LOG(LOG_WARNING, "no scene found for audio routing");
	}

	CAPCAST_LOG(LOG_INFO, "audio direct-routed to %s (%d scenes)",
		    qUtf8Printable(dev_id),
		    (int)g_state.router_attachments.size());
	return {};
#else
	Q_UNUSED(device_id);
	return QStringLiteral("直接音频路由仅支持 Windows");
#endif
}

static void stop_audio_routing()
{
	/* 移除并销毁场景上的过滤器 */
	for (const auto &att : g_state.router_attachments) {
		if (att.host && att.filter) {
			obs_source_filter_remove(att.host, att.filter);
			obs_source_release(att.filter);
		}
	}
	g_state.router_attachments.clear();

#ifdef _WIN32
	wasapi_shutdown();
	CAPCAST_LOG(LOG_INFO, "audio direct-routing stopped");
#endif
}

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

/* 自动选择"最像采集卡的副屏": 关键字优先, 否则第一个非主屏 */
int pick_default_screen()
{
	const auto screens = enum_screens();
	if (screens.isEmpty())
		return -1;

	/* 关键字优先 */
	static const QStringList keywords = {
		QStringLiteral("capture"), QStringLiteral("elgato"),
		QStringLiteral("hd60"),   QStringLiteral("hdx"),
		QStringLiteral("采集"),   QStringLiteral("录屏"),
	};
	for (const auto &sc : screens) {
		if (sc.isPrimary)
			continue;
		const QString lower = sc.name.toLower();
		for (const auto &kw : keywords) {
			if (lower.contains(kw))
				return sc.index;
		}
	}

	/* 兜底: 第一个非主屏 */
	for (const auto &sc : screens) {
		if (!sc.isPrimary)
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

	/* CLSID_MMDeviceEnumerator 本地定义(BCDE0395-E52F-467C-8E3D-C4579291692E),
	 * 避免依赖 uuid.lib 的符号解析(现代 SDK 该符号不在默认链接库中) */
	static const GUID CLSID_MMDeviceEnumerator_Local = {
		0xBCDE0395, 0xE52F, 0x467C,
		{0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};

	IMMDeviceEnumerator *enumerator = nullptr;
	HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator_Local, nullptr,
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

/* 自动选择"最像采集卡的音频输出端点": 关键字优先, 否则第一个非默认音箱 */
CapCastAudioDevice pick_default_audio()
{
	const auto devices = enum_audio_devices();
	if (devices.isEmpty())
		return {};

	/* 排除明显是音箱/耳机的设备 */
	static const QStringList skip_keywords = {
		QStringLiteral("speaker"), QStringLiteral("耳机"),
		QStringLiteral("音箱"),   QStringLiteral("headphone"),
		QStringLiteral("realtek"), QStringLiteral("sound card"),
		QStringLiteral("声卡"),
	};
	/* 采集卡关键字优先 */
	static const QStringList capture_keywords = {
		QStringLiteral("capture"), QStringLiteral("elgato"),
		QStringLiteral("hd60"),   QStringLiteral("hdx"),
		QStringLiteral("采集"),   QStringLiteral("video capture"),
	};

	/* 1. 名字含采集卡关键字 */
	for (const auto &d : devices) {
		const QString lower = d.name.toLower();
		for (const auto &kw : capture_keywords) {
			if (lower.contains(kw))
				return d;
		}
	}

	/* 2. 第一个非音箱/耳机设备 */
	for (const auto &d : devices) {
		const QString lower = d.name.toLower();
		bool skip = false;
		for (const auto &kw : skip_keywords) {
			if (lower.contains(kw)) {
				skip = true;
				break;
			}
		}
		if (!skip)
			return d;
	}

	/* 3. 兜底第一个 */
	return devices.first();
}

/* ================= 一键推流 ================= */

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

	/* 3. 直接软路由全部音频到采集卡(WASAPI 直出, 不动 OBS 混音器) */
	const QString err = start_audio_routing(audio_device_id);
	if (!err.isEmpty()) {
		g_state.last_error =
			QStringLiteral("投影已打开, 但音频路由失败: %1").arg(err);
	}

	g_state.active = true;
	return {};
}

void stop_output()
{
	close_projector_windows();
	stop_audio_routing();
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

/* ================= 配置持久化 =================
 * 注意: 每次写入后立即 config_save, 保证设置持久化到磁盘 */
static void cfg_flush()
{
	config_save(user_config());
}

QString cfg_display_mode()
{
	return config_get_string(user_config(), CFG_SECTION, "DisplayMode");
}
void cfg_set_display_mode(const QString &m)
{
	config_set_string(user_config(), CFG_SECTION, "DisplayMode",
			  qUtf8Printable(m));
	cfg_flush();
}
int cfg_display_index()
{
	return (int)config_get_int(user_config(), CFG_SECTION, "DisplayIndex");
}
void cfg_set_display_index(int idx)
{
	config_set_int(user_config(), CFG_SECTION, "DisplayIndex", idx);
	cfg_flush();
}

QString cfg_audio_mode()
{
	return config_get_string(user_config(), CFG_SECTION, "AudioMode");
}
void cfg_set_audio_mode(const QString &m)
{
	config_set_string(user_config(), CFG_SECTION, "AudioMode",
			  qUtf8Printable(m));
	cfg_flush();
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
	cfg_flush();
}

QString cfg_source()
{
	return config_get_string(user_config(), CFG_SECTION, "Source");
}
void cfg_set_source(const QString &s)
{
	config_set_string(user_config(), CFG_SECTION, "Source",
			  qUtf8Printable(s));
	cfg_flush();
}

bool cfg_auto_start()
{
	return config_get_bool(user_config(), CFG_SECTION, "AutoStart");
}
void cfg_set_auto_start(bool v)
{
	config_set_bool(user_config(), CFG_SECTION, "AutoStart", v);
	cfg_flush();
}

} // namespace capcast
