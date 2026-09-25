/*
 * capcast-core.cpp - CapCast 采集卡一键推流插件 核心实现
 *
 * 平台: Windows (显示扩展/音频枚举为 Win32 API, 跨平台可降级为仅 Qt 屏幕)
 * 依赖: libobs + obs-frontend-api + Qt6 Widgets
 *
 * 音频路由 —— 独立于 OBS 混音器运行:
 *   通过 obs_add_raw_audio_callback() 订阅 OBS 主混音(mix 0)的"成品音频",
 *   并让 OBS 按采集卡端点的真实参数(采样率/声道)完成混音与重采样;
 *   插件只负责 float -> 设备原始格式(8/16/24/32bit/float)的落格式 + 一次写入。
 *
 *   相比旧实现(逐源挂 filter、各源各自 memcpy 进 WASAPI):
 *     - 旧实现把每个源顺序写进设备缓冲 -> 各源被时间轴错开, 听感即"杂音";
 *       现在订阅的是 OBS 已混好的单路音频, 天然不存在此问题。
 *     - 旧实现硬编码按 float 喂设备, 与采集卡真实 mix format 错配 -> 白噪;
 *       现在严格按 GetMixFormat() 的位深/格式转换。
 *     - 旧实现 buffer 请求 10000000hns(=1秒) -> 延迟严重; 现在 + 自适应积压控制,
 *       把实际延迟钉在一个音频块的量级。
 *   全程不改 OBS 混音器/监听设备设置, 也不走 audio_monitor。
 */
#include "capcast-core.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.h>
#include <util/config-file.h> /* obs 31: 原 util/config.h 已合并到此 */
#include <media-io/audio-io.h>

#include <QGuiApplication>
#include <QScreen>
#include <QApplication>
#include <QWidget>
#include <QPointer>
#include <QSet>
#include <QHash>

#include <atomic>

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
#include <atomic>
#include <vector>
#endif

/* ================= 内部状态 ================= */
namespace {

/* 投屏 与 音频映射 各自的运行状态(两项互不依赖, 可单独开关)。
 * 读写跨 UI 线程 / OBS 音频线程 / 模块卸载路径, 故用 atomic。 */
std::atomic<bool> g_proj_active{false};
std::atomic<bool> g_audio_active{false};

/* 本插件自己打开的那个全屏投影窗口。
 *
 * OBS 没有提供关闭投影的 API, 只能自己定位窗口。旧实现无差别遍历
 * QApplication::topLevelWidgets() 关闭所有 OBSProjector —— 那会把用户
 * 自己在 OBS 里打开的预览/多视图投影一并关掉。
 *
 * 这里用 QPointer 记住"我们打开的那一个":
 *   - obs_frontend_open_projector() 内部是阻塞式调用(WaitConnection),
 *     返回时窗口已创建完毕, 因此可以在调用后立刻做"前后差集"定位它;
 *   - QPointer 保证窗口被用户手动关掉后自动置空, 不会留下悬垂指针。 */
QPointer<QWidget> g_projector;

/* 收集当前所有 OBS 全屏投影窗口(OBSProjector 顶层窗口) */
static QSet<QWidget *> collect_projectors()
{
	QSet<QWidget *> out;
	if (!qApp)
		return out;
	for (QWidget *w : QApplication::topLevelWidgets()) {
		if (w && w->inherits("OBSProjector"))
			out.insert(w);
	}
	return out;
}

/* 送到采集卡的音量(百分比, 100 = 原始主混音音量)。
 * 音频线程(on_master_audio)读取, UI 线程(设置面板)写入, 故用 atomic。 */
std::atomic<double> g_volume_pct{50.0};

/* 当前订阅的 mix 索引(= 音轨 - 1)。
 * obs_remove_raw_audio_callback 必须用与订阅时相同的 mix_idx 才能移除成功,
 * 因此把它存下来, 避免运行期间改了配置导致取消订阅失败(音频关不掉)。 */
std::atomic<int> g_mix_idx{0};

/* 音轨(1-6) -> OBS mix 索引(0-5) */
size_t mix_index_from_track(int track)
{
	if (track < 1)
		track = 1;
	if ((size_t)track > MAX_AUDIO_MIXES)
		track = (int)MAX_AUDIO_MIXES;
	return (size_t)(track - 1);
}

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

#ifdef _WIN32
QString wide_to_qstring(const wchar_t *w)
{
	return w ? QString::fromWCharArray(w) : QString();
}
#endif

} // namespace

/* ================= WASAPI 直出(独立于 OBS 混音器) =================
 * 数据通路:
 *   OBS 主混音(mix 0) --obs_add_raw_audio_callback--> on_master_audio()
 *     --> wasapi_write_audio() -> float 转设备格式 -> 一次 memcpy 进设备缓冲
 */
#ifdef _WIN32

/* WASAPI 渲染端点(shared mode, 严格使用设备混音格式) */
struct WasapiOutput {
	IMMDevice *device = nullptr;
	IAudioClient *client = nullptr;
	IAudioRenderClient *render = nullptr;
	UINT32 buffer_frames = 0;
	UINT32 channels = 2;
	UINT32 sample_rate = 48000;
	UINT32 dev_bits = 32;  /* 设备每采样位深: 8/16/24/32 */
	bool dev_float = true; /* 设备是否为 32bit IEEE float */
	UINT32 frame_bytes = 8; /* 一帧(全部声道)的字节数 */

	UINT32 obs_block = 0; /* OBS 每次音频回调给的帧数, 首次回调时学到 */

	/* 保护下面的 COM 对象与暂存缓冲:
	 * on_master_audio 在 OBS 音频线程执行, 而 start/stop 在 UI 线程,
	 * 需要互斥。用 recursive_mutex: wasapi_start 失败路径会内调
	 * wasapi_shutdown, 递归加锁避免自死锁。 */
	std::recursive_mutex mtx;
	std::atomic<bool> active{false};

	/* float -> 设备格式 的暂存缓冲 */
	std::vector<uint8_t> scratch;
};

static WasapiOutput g_wasapi;

static void wasapi_shutdown()
{
	std::lock_guard<std::recursive_mutex> lk(g_wasapi.mtx);
	if (g_wasapi.render)
		g_wasapi.render->Release();
	g_wasapi.render = nullptr;
	if (g_wasapi.client)
		g_wasapi.client->Release();
	g_wasapi.client = nullptr;
	if (g_wasapi.device)
		g_wasapi.device->Release();
	g_wasapi.device = nullptr;
	g_wasapi.buffer_frames = 0;
	g_wasapi.obs_block = 0;
	g_wasapi.scratch.clear();
	g_wasapi.active = false;
}

/* KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = {00000003-0000-0010-8000-00AA00389B71}
 * 本地定义, 避免依赖 ksmedia.h / uuid.lib 符号 */
static const GUID k_subtype_ieee_float = {
	0x00000003, 0x0000, 0x0010,
	{0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

/* 判断设备混音格式是否为 32bit IEEE float */
static bool wave_is_float(const WAVEFORMATEX *wf)
{
	if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
		return true;
	if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
		const WAVEFORMATEXTENSIBLE *wfe =
			reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(wf);
		return IsEqualGUID(wfe->SubFormat, k_subtype_ieee_float) != FALSE;
	}
	return false;
}

/* 设备声道数 -> OBS speaker 布局 */
static enum speaker_layout layout_from_channels(UINT32 n)
{
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
}

static bool wasapi_start(const char *device_id)
{
	std::lock_guard<std::recursive_mutex> lk(g_wasapi.mtx);
	HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	(void)hr;

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

	/* shared mode 必须使用 GetMixFormat() 返回的确切格式 */
	WAVEFORMATEX *wfex = nullptr;
	hr = g_wasapi.client->GetMixFormat(&wfex);
	if (FAILED(hr) || !wfex) {
		CAPCAST_LOG(LOG_ERROR, "wasapi: GetMixFormat failed");
		wasapi_shutdown();
		return false;
	}
	g_wasapi.sample_rate = wfex->nSamplesPerSec;
	g_wasapi.channels = wfex->nChannels;
	g_wasapi.dev_float = wave_is_float(wfex);
	if (g_wasapi.dev_float) {
		g_wasapi.dev_bits = 32;
	} else {
		switch (wfex->wBitsPerSample) {
		case 8:
			g_wasapi.dev_bits = 8;
			break;
		case 24:
			g_wasapi.dev_bits = 24;
			break;
		case 32:
			g_wasapi.dev_bits = 32;
			break;
		default:
			g_wasapi.dev_bits = 16;
			break;
		}
	}
	g_wasapi.frame_bytes =
		g_wasapi.channels * (g_wasapi.dev_bits / 8);

	/* 缓冲申请 100ms: 仅作为抖动余量(headroom)。
	 * 实际延迟由"积压量(padding)"决定, 由下面的自适应控制压在低位,
	 * 而不是像旧实现那样把缓冲本身开成 1 秒导致延迟爆炸。 */
	hr = g_wasapi.client->Initialize(
		AUDCLNT_SHAREMODE_SHARED, 0, 1000000 /* 100ms */, 0, wfex,
		nullptr);
	CoTaskMemFree(wfex);
	if (FAILED(hr)) {
		CAPCAST_LOG(LOG_ERROR, "wasapi: initialize failed (0x%08lX)",
			    hr);
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

	hr = g_wasapi.client->Start();
	if (FAILED(hr)) {
		wasapi_shutdown();
		return false;
	}
	g_wasapi.active = true;
	CAPCAST_LOG(LOG_INFO,
		    "wasapi started: %uHz %uch %ubit%s, buffer %u frames",
		    g_wasapi.sample_rate, g_wasapi.channels,
		    g_wasapi.dev_bits, g_wasapi.dev_float ? " float" : " int",
		    g_wasapi.buffer_frames);
	return true;
}

/* 交错 float32 -> 设备原始格式, 同时应用输出音量增益 */
static void convert_float_to_device(const float *src, uint8_t *dst,
				    uint32_t frames, float gain)
{
	const UINT32 total = frames * g_wasapi.channels;

	if (g_wasapi.dev_float) {
		float *p = reinterpret_cast<float *>(dst);
		if (gain == 1.f) { /* 快速路径: 增益为 1 时直接拷贝 */
			memcpy(dst, src, (size_t)total * sizeof(float));
			return;
		}
		for (UINT32 i = 0; i < total; ++i) {
			const float v = src[i] * gain;
			p[i] = v < -1.f ? -1.f : (v > 1.f ? 1.f : v);
		}
		return;
	}

	switch (g_wasapi.dev_bits) {
	case 8: { /* unsigned 8bit */
		for (UINT32 i = 0; i < total; ++i) {
			float v = src[i] * gain;
			v = v < -1.f ? -1.f : (v > 1.f ? 1.f : v);
			dst[i] = (uint8_t)(int)((v + 1.f) * 127.5f);
		}
		break;
	}
	case 16: {
		int16_t *p = reinterpret_cast<int16_t *>(dst);
		for (UINT32 i = 0; i < total; ++i) {
			float v = src[i] * gain;
			v = v < -1.f ? -1.f : (v > 1.f ? 1.f : v);
			p[i] = (int16_t)(v * 32767.f);
		}
		break;
	}
	case 24: {
		for (UINT32 i = 0; i < total; ++i) {
			float v = src[i] * gain;
			v = v < -1.f ? -1.f : (v > 1.f ? 1.f : v);
			const int32_t s = (int32_t)(v * 8388607.f);
			uint8_t *p = dst + (size_t)i * 3;
			p[0] = (uint8_t)(s & 0xFF);
			p[1] = (uint8_t)((s >> 8) & 0xFF);
			p[2] = (uint8_t)((s >> 16) & 0xFF);
		}
		break;
	}
	default: { /* 32bit int */
		int32_t *p = reinterpret_cast<int32_t *>(dst);
		for (UINT32 i = 0; i < total; ++i) {
			float v = src[i] * gain;
			v = v < -1.f ? -1.f : (v > 1.f ? 1.f : v);
			p[i] = (int32_t)(v * 2147483647.f);
		}
		break;
	}
	}
}

/* 把 OBS 主混音(交错 float32, 已是设备采样率/声道)写入采集卡端点 */
static void wasapi_write_audio(const float *src, uint32_t frames)
{
	std::lock_guard<std::recursive_mutex> lk(g_wasapi.mtx);
	if (!g_wasapi.active || !g_wasapi.render || !src || frames == 0)
		return;

	/* 1) float -> 设备原始格式(同时应用用户设定的输出音量) */
	const float gain =
		(float)(g_volume_pct.load(std::memory_order_relaxed) / 100.0);
	const size_t need =
		(size_t)frames * g_wasapi.channels * (g_wasapi.dev_bits / 8);
	if (g_wasapi.scratch.size() < need)
		g_wasapi.scratch.resize(need);
	convert_float_to_device(src, g_wasapi.scratch.data(), frames, gain);
	const uint8_t *data = g_wasapi.scratch.data();

	/* 2) 目标积压 = 一个 OBS 音频块。让实际延迟贴着"一块"的量级,
	 *    而不是整套缓冲, 也不会像旧实现那样越跑越慢。 */
	if (g_wasapi.obs_block == 0)
		g_wasapi.obs_block = frames;
	const UINT32 target = g_wasapi.obs_block < g_wasapi.buffer_frames
				      ? g_wasapi.obs_block
				      : g_wasapi.buffer_frames / 2;

	UINT32 pad = 0;
	if (FAILED(g_wasapi.client->GetCurrentPadding(&pad)))
		return;

	/* 3) 自适应: 设备时钟略慢于 OBS 时积压会缓慢上涨,
	 *    每次最多丢 1ms, 听感无感, 但能长期把延迟钉在低位。 */
	if (pad > target) {
		UINT32 excess = pad - target;
		const UINT32 max_adj = g_wasapi.sample_rate / 1000; /* 1ms */
		if (excess > max_adj)
			excess = max_adj;
		if (excess >= frames)
			return;
		data += (size_t)excess * g_wasapi.frame_bytes;
		frames -= excess;
	}

	/* 4) 写入设备环形缓冲(不阻塞 OBS 音频线程) */
	UINT32 remaining = frames;
	while (remaining > 0) {
		if (FAILED(g_wasapi.client->GetCurrentPadding(&pad)))
			return;
		const UINT32 avail = g_wasapi.buffer_frames - pad;
		if (avail == 0)
			break; /* 缓冲满: 丢弃剩余 */
		const UINT32 chunk = remaining < avail ? remaining : avail;

		BYTE *dst = nullptr;
		/* 取缓冲失败: 放弃本帧剩余部分。不能阻塞 OBS 音频线程,
		 * 也不能对未成功 GetBuffer 的句柄调用 ReleaseBuffer。 */
		if (FAILED(g_wasapi.render->GetBuffer(chunk, &dst)))
			return;
		memcpy(dst, data, (size_t)chunk * g_wasapi.frame_bytes);
		data += (size_t)chunk * g_wasapi.frame_bytes;
		remaining -= chunk;
		g_wasapi.render->ReleaseBuffer(chunk, 0);
	}
}

/* OBS 主混音回调(在 OBS 音频线程执行) */
static void on_master_audio(void *param, size_t mix_idx,
			    struct audio_data *audio)
{
	(void)param;
	(void)mix_idx;
	if (!audio || audio->frames == 0 || !audio->data[0])
		return;
	/* conversion 已让 OBS 输出"设备采样率 + 设备声道 + 交错 float32" */
	wasapi_write_audio(reinterpret_cast<const float *>(audio->data[0]),
			   audio->frames);
}

#endif /* _WIN32 */

/* ================= 音频路由启停 ================= */

/* 清理旧版本残留在场景/源上的 capcast-router 过滤器
 * (旧实现逐源挂 filter 做直出; 新实现改为订阅主混音, 不再需要过滤器)。
 * 注意: 绝不能在 obs_enum_sources 的回调里操作源 —— 枚举期间 OBS 持有
 * 源列表锁。因此先"收集"源列表, 枚举结束后再"处理"。 */
#ifdef _WIN32
static void purge_legacy_router_filters()
{
	QVector<obs_source_t *> hosts;
	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			auto *list =
				static_cast<QVector<obs_source_t *> *>(param);
			obs_source_t *f = obs_source_get_filter_by_name(
				source, "capcast-router");
			if (f) {
				obs_source_release(f);
				list->append(source);
			}
			return true;
		},
		&hosts);

	for (obs_source_t *host : hosts) {
		obs_source_t *f =
			obs_source_get_filter_by_name(host, "capcast-router");
		if (!f)
			continue;
		obs_source_filter_remove(host, f);
		obs_source_release(f);
	}
	if (!hosts.isEmpty())
		CAPCAST_LOG(LOG_INFO, "purged %d legacy router filter(s)",
			    (int)hosts.size());
}
#endif

/* 开始音频路由: 订阅 OBS 主混音 -> 直出采集卡端点
 * 完全不改动 OBS 混音器音量/静音/监听设备设置, 也不使用 audio_monitor。 */
static QString start_audio_routing(const QString &device_id)
{
#ifdef _WIN32
	const QString dev_id = device_id.isEmpty() ? QStringLiteral("default")
						   : device_id;

	purge_legacy_router_filters();

	if (!wasapi_start(qUtf8Printable(dev_id)))
		return QStringLiteral("音频设备打开失败(%1)").arg(dev_id);

	if (!obs_get_audio()) {
		wasapi_shutdown();
		return QStringLiteral("OBS 音频未初始化");
	}

	/* 载入输出音量(默认 50%) */
	g_volume_pct.store(capcast::cfg_audio_volume(),
			   std::memory_order_relaxed);

	/* 载入订阅音轨(默认 1 -> mix 索引 0) */
	const int track = capcast::cfg_audio_track();
	g_mix_idx.store((int)mix_index_from_track(track),
			std::memory_order_relaxed);

	/* 让 OBS 直接用采集卡端点的采样率/声道把主混音转换成交错 float32,
	 * 插件只做最后一次"float -> 设备原始格式"的落格式。
	 * 这样混音、重采样、声道映射全部由 OBS 音频引擎负责。 */
	struct audio_convert_info conv{};
	conv.samples_per_sec = g_wasapi.sample_rate;
	conv.format = AUDIO_FORMAT_FLOAT;
	conv.speakers = layout_from_channels(g_wasapi.channels);
	conv.allow_clipping = true;
	obs_add_raw_audio_callback((size_t)g_mix_idx.load(std::memory_order_relaxed),
				   &conv, on_master_audio, nullptr);

	CAPCAST_LOG(LOG_INFO,
		    "audio routing started: mix %d (track %d) -> %s (%uHz %uch %ubit%s), volume %.0f%%",
		    g_mix_idx.load(std::memory_order_relaxed), track,
		    qUtf8Printable(dev_id), g_wasapi.sample_rate,
		    g_wasapi.channels, g_wasapi.dev_bits,
		    g_wasapi.dev_float ? " float" : " int",
		    g_volume_pct.load(std::memory_order_relaxed));
	return {};
#else
	Q_UNUSED(device_id);
	return QStringLiteral("直接音频路由仅支持 Windows");
#endif
}

static void stop_audio_routing()
{
#ifdef _WIN32
	/* 必须用与订阅时相同的 mix 索引, 否则取消订阅失败(表现为停不下来) */
	obs_remove_raw_audio_callback(
		(size_t)g_mix_idx.load(std::memory_order_relaxed),
		on_master_audio, nullptr);
	wasapi_shutdown();
	CAPCAST_LOG(LOG_INFO, "audio routing stopped (mix %d)",
		    g_mix_idx.load(std::memory_order_relaxed));
#endif
}

/* ================= 显示器 ================= */

#ifdef _WIN32
/* ---- Windows: 查询每个显示器的真实物理分辨率 ----
 *
 * 为什么需要:
 *   Qt 的 QScreen::size() 返回"逻辑像素"。若宿主进程(OBS)不是 per-monitor
 *   DPI 感知进程, Windows 会对它做 DPI 虚拟化, 此时 size() 是已被系统缩放
 *   除过的值, 而 devicePixelRatio() 恒为 1.0 —— 于是 4K 屏(3840x2160)在 300%
 *   缩放下会被报成 1280x720, 且凭 Qt 自身无法还原。
 *
 *   EnumDisplaySettings(ENUM_CURRENT_SETTINGS) 返回的是显示器当前的真实
 *   显示模式(物理像素), 不受调用进程的 DPI 感知状态影响, 因此拿它作为
 *   物理分辨率的权威来源最稳妥 —— 既能在"非感知"时还原 4K, 也不会在
 *   "已感知"(此时 Qt 报的就是物理值)时错误地再乘一次缩放。 */
namespace {

struct WinMonitorInfo {
	QHash<QString, QSize> physicalByName; /* "\\.\DISPLAY2" -> 物理像素 */
	QVector<QSize> physicalByOrder;       /* EnumDisplayMonitors 顺序 */
};

BOOL CALLBACK capcast_monitor_enum_proc(HMONITOR hmon, HDC, LPRECT, LPARAM lparam)
{
	auto *out = reinterpret_cast<WinMonitorInfo *>(lparam);

	MONITORINFOEXW info{};
	info.cbSize = sizeof(info);
	if (!GetMonitorInfoW(hmon, &info))
		return TRUE;

	QSize phys;
	DEVMODEW dm{};
	dm.dmSize = sizeof(dm);
	if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
		phys = QSize(static_cast<int>(dm.dmPelsWidth),
			     static_cast<int>(dm.dmPelsHeight));
	}

	const QString dev = QString::fromWCharArray(info.szDevice);
	out->physicalByName.insert(dev, phys);
	out->physicalByOrder.append(phys);
	return TRUE;
}

WinMonitorInfo win_monitor_info()
{
	WinMonitorInfo out;
	if (!EnumDisplayMonitors(nullptr, nullptr,
				 capcast_monitor_enum_proc,
				 reinterpret_cast<LPARAM>(&out)))
		return {};
	return out;
}

} // namespace
#endif // _WIN32

namespace capcast {

QVector<CapCastScreen> enum_screens()
{
	QVector<CapCastScreen> out;
	const auto screens = QGuiApplication::screens();

#ifdef _WIN32
	const WinMonitorInfo win = win_monitor_info();
#endif

	for (int i = 0; i < screens.size(); ++i) {
		const QScreen *s = screens.at(i);
		CapCastScreen sc;
		sc.index = i;
		sc.name = s->name();
		sc.isPrimary = (s == QGuiApplication::primaryScreen());

	const QSize logical = s->size();

	/* 物理分辨率: 以 Windows 报告的当前显示模式为准权威值。
	 * 拿不到(非 Windows 或 API 失败)时才退回 Qt 的 size()。 */
	QSize physical = logical;
#ifdef _WIN32
	{
		bool have = false;
		QSize p;
		auto it = win.physicalByName.constFind(sc.name);
		if (it != win.physicalByName.constEnd()) {
			p = *it;
			have = true;
		} else if (i < win.physicalByOrder.size()) {
			/* 设备名对不上(Qt 可能返回友好名)时, 按枚举顺序兜底 */
			p = win.physicalByOrder.at(i);
			have = true;
		}
		if (have && p.width() > 0 && p.height() > 0)
			physical = p;
	}
#endif

	/* 缩放倍数 = 物理 / 逻辑。OBS 已是 DPI 感知时两者相等 -> 1.0(不会误放大);
	 * 非感知时 4K@300% 会得到 3.0, 正好解释"为何显示成 1280x720"。 */
	const double scale = (logical.width() > 0)
				     ? static_cast<double>(physical.width()) /
					       static_cast<double>(logical.width())
				     : 1.0;

	sc.logical = logical;
	sc.physical = physical;
	sc.scale = (scale >= 1.0) ? scale : 1.0;

		int refresh = static_cast<int>(s->refreshRate());
		sc.geometry = QStringLiteral("%1x%2@%3")
				      .arg(physical.width())
				      .arg(physical.height())
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

/* ================= 投屏 / 音频映射(各自独立控制) ================= */

bool is_projector_active()
{
	return g_proj_active.load(std::memory_order_relaxed);
}

bool is_audio_active()
{
	return g_audio_active.load(std::memory_order_relaxed);
}

bool is_output_active()
{
	return is_projector_active() || is_audio_active();
}

/* 解析目标屏幕序号: <0 表示自动选择 */
static int resolve_screen_index()
{
	if (cfg_display_mode() == QStringLiteral("manual"))
		return cfg_display_index();
	return -1;
}

/* 解析目标音频设备 ID: 空串表示自动选择 */
static QString resolve_audio_device()
{
	if (cfg_audio_mode() == QStringLiteral("manual"))
		return cfg_audio_device_id();
	return QString();
}

static CapCastProjectorSource resolve_source()
{
	return (cfg_source() == QStringLiteral("preview"))
		       ? CapCastProjectorSource::Preview
		       : CapCastProjectorSource::Program;
}

QString start_projector(int screen_index, CapCastProjectorSource source)
{
	/* 已在运行则先停掉旧的, 避免残留多个投影窗口 */
	if (g_proj_active.load(std::memory_order_relaxed))
		stop_projector();

	/* 未指定 -> 自动选择 */
	if (screen_index < 0)
		screen_index = pick_default_screen();

	auto screens = enum_screens();

	/* 找不到副屏且允许自动扩展 -> 把显示拓扑设为"扩展"后重试一次 */
	if ((screen_index < 0 || screen_index >= screens.size()) &&
	    cfg_auto_extend()) {
		CAPCAST_LOG(LOG_INFO,
			    "capture screen not found, extending display...");
		ensure_display_extend();
		screens = enum_screens();
		screen_index = pick_default_screen();
	}

	if (screen_index < 0 || screen_index >= screens.size()) {
		return QStringLiteral("采集卡副屏未找到(序号 %1 不存在)")
			.arg(screen_index);
	}

	/* obs 31: type 为字符串, "StudioProgram"=节目, nullptr=默认预览 */
	const char *projector_type =
		(source == CapCastProjectorSource::Program)
			? "StudioProgram"
			: nullptr;

	/* 打开前快照已有投影, 打开后按差集定位"我们新开的这一个"。
	 * obs_frontend_open_projector() 内部是阻塞调用, 返回时窗口已创建完。 */
	const QSet<QWidget *> before = collect_projectors();
	obs_frontend_open_projector(projector_type, screen_index, nullptr,
				    nullptr);

	QWidget *mine = nullptr;
	const QSet<QWidget *> after = collect_projectors();
	for (QWidget *w : after) {
		if (!before.contains(w)) {
			mine = w;
			break;
		}
	}

	/* 没检测到新增窗口 -> 认定打开失败并如实报错。
	 *
	 * 关键: 这种情况绝不记录任何窗口、也不置 active —— 否则会把用户
	 * 自己打开的投影误当成"我们的", 停止时就会误关掉它(旧实现正是
	 * 无差别关闭所有 OBSProjector, 会误伤用户手动开的预览/多视图投影)。 */
	if (!mine) {
		CAPCAST_LOG(LOG_WARNING, "projector window was not created");
		return QStringLiteral("投影窗口未能打开(OBS 未创建新的投影窗口)");
	}

	g_projector = mine;
	g_proj_active.store(true, std::memory_order_relaxed);
	{
		const CapCastScreen sc = screens.at(screen_index);
		CAPCAST_LOG(LOG_INFO,
			    "projector started on screen %d: %s physical %dx%d "
			    "(Qt logical %dx%d, system scale %.0f%%)",
			    screen_index, qUtf8Printable(sc.name),
			    sc.physical.width(), sc.physical.height(),
			    sc.logical.width(), sc.logical.height(),
			    sc.scale * 100.0);
	}
	return {};
}

void stop_projector()
{
	/* 只关我们自己打开的那一个。
	 *
	 * QPointer 为空说明它已被用户或 OBS 关掉了 —— 那就什么都不用做。
	 * 绝不去关闭别的投影窗口。
	 *
	 * qApp 为空(卸载/退出尾声)时同样不碰 Qt: OBS 正在销毁窗口,
	 * 此时 close() 会把事件派发到已析构对象导致崩溃(旧版退出必崩的根因),
	 * 交给 OBS 自己销毁即可。 */
	if (!g_projector.isNull() && qApp) {
		CAPCAST_LOG(LOG_INFO, "closing our projector window");
		g_projector->close();
	}

	g_projector = nullptr;
	g_proj_active.store(false, std::memory_order_relaxed);
}

QString start_audio_mapping(const QString &audio_device_id)
{
	if (g_audio_active.load(std::memory_order_relaxed))
		stop_audio_mapping();

	/* 如实返回错误 —— 旧实现把失败吞掉(恒 return {}), 导致音频其实没
	 * 起来而界面显示正常。 */
	const QString err = start_audio_routing(audio_device_id);
	if (!err.isEmpty()) {
		g_audio_active.store(false, std::memory_order_relaxed);
		return err;
	}
	g_audio_active.store(true, std::memory_order_relaxed);
	return {};
}

void stop_audio_mapping()
{
	stop_audio_routing();
	g_audio_active.store(false, std::memory_order_relaxed);
}

QString start_enabled()
{
	const bool want_proj = cfg_enable_projector();
	const bool want_audio = cfg_enable_audio();

	if (!want_proj && !want_audio) {
		CAPCAST_LOG(LOG_INFO,
			    "start skipped: neither projector nor audio enabled");
		return {};
	}

	QStringList errors;

	if (want_proj) {
		const QString e =
			start_projector(resolve_screen_index(), resolve_source());
		if (!e.isEmpty())
			errors << QStringLiteral("投屏: %1").arg(e);
	}

	if (want_audio) {
		const QString e = start_audio_mapping(resolve_audio_device());
		if (!e.isEmpty())
			errors << QStringLiteral("音频映射: %1").arg(e);
	}

	/* 部分失败时, 成功那一项保持运行, 只把失败原因如实返回给界面 */
	return errors.join(QStringLiteral("；"));
}

void stop_all()
{
	stop_projector();
	stop_audio_mapping();
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

double cfg_audio_volume()
{
	/* 未设置过 -> 默认 50%(用 has_user_value 判断, 避免把用户主动设的 0
	 * 当成"未设置"而重置回 50) */
	if (!config_has_user_value(user_config(), CFG_SECTION, "AudioVolume"))
		return 50.0;
	return config_get_double(user_config(), CFG_SECTION, "AudioVolume");
}
void cfg_set_audio_volume(double percent)
{
	config_set_double(user_config(), CFG_SECTION, "AudioVolume", percent);
	cfg_flush();
}

void set_output_volume(double percent)
{
	/* 仅运行时生效: 音频线程下一次回调就会用新的增益, 无需重启路由。
	 * 不在此处落盘 —— 拖动滑块会高频触发, 持久化交给设置面板保存时做。 */
	g_volume_pct.store(percent, std::memory_order_relaxed);
}

int cfg_audio_track()
{
	const int t =
		(int)config_get_int(user_config(), CFG_SECTION, "AudioTrack");
	/* 未设置过或值非法 -> 默认音轨 1 */
	if (t < 1 || (size_t)t > MAX_AUDIO_MIXES)
		return 1;
	return t;
}
void cfg_set_audio_track(int track)
{
	config_set_int(user_config(), CFG_SECTION, "AudioTrack", track);
	cfg_flush();
}

void apply_audio_track(int track)
{
#ifdef _WIN32
	if (!g_wasapi.active)
		return; /* 未运行: 下次开始时生效 */

	const size_t new_mix = mix_index_from_track(track);
	const size_t old_mix =
		(size_t)g_mix_idx.load(std::memory_order_relaxed);
	if (new_mix == old_mix)
		return;

	/* 先用原索引取消订阅, 再按新索引重新订阅 */
	obs_remove_raw_audio_callback(old_mix, on_master_audio, nullptr);
	g_mix_idx.store((int)new_mix, std::memory_order_relaxed);

	if (obs_get_audio()) {
		struct audio_convert_info conv{};
		conv.samples_per_sec = g_wasapi.sample_rate;
		conv.format = AUDIO_FORMAT_FLOAT;
		conv.speakers = layout_from_channels(g_wasapi.channels);
		conv.allow_clipping = true;
		obs_add_raw_audio_callback(new_mix, &conv, on_master_audio,
					   nullptr);
	}
	CAPCAST_LOG(LOG_INFO, "audio track switched: mix %d -> mix %d",
		    (int)old_mix, (int)new_mix);
#else
	Q_UNUSED(track);
#endif
}

void stop_audio_only()
{
	/* 只停音频并释放自有资源, 不触碰任何 Qt 窗口(原因见头文件注释)。
	 *
	 * 注意: 卸载时调用 obs_remove_raw_audio_callback 是安全的 ——
	 * obs_shutdown() 的执行顺序是 stop_video() -> stop_audio() ->
	 * stop_hotkeys() -> free_module(), 即音频子系统先于模块卸载被销毁;
	 * stop_audio() 会把 obs->audio.audio 置 NULL, 而
	 * audio_output_disconnect() 开头就有 `if (!audio) return;`,
	 * 因此这里的取消订阅会直接空操作返回, 不会踩空指针。
	 * (audio_output_close() 也已经把我们的回调连接一并释放了。)
	 *
	 * 投影窗口则由 OBS 自己在退出时销毁。 */
	stop_audio_routing();
	g_audio_active.store(false, std::memory_order_relaxed);
	g_proj_active.store(false, std::memory_order_relaxed);
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

/* 功能勾选默认 true(首次安装即可用)。
 * 用 has_user_value 区分"用户主动取消勾选(false)"与"从未设置过",
 * 否则每次启动都会把用户取消的勾又勾回来。 */
bool cfg_enable_projector()
{
	if (!config_has_user_value(user_config(), CFG_SECTION,
				   "EnableProjector"))
		return true;
	return config_get_bool(user_config(), CFG_SECTION, "EnableProjector");
}
void cfg_set_enable_projector(bool v)
{
	config_set_bool(user_config(), CFG_SECTION, "EnableProjector", v);
	cfg_flush();
}

bool cfg_enable_audio()
{
	if (!config_has_user_value(user_config(), CFG_SECTION, "EnableAudio"))
		return true;
	return config_get_bool(user_config(), CFG_SECTION, "EnableAudio");
}
void cfg_set_enable_audio(bool v)
{
	config_set_bool(user_config(), CFG_SECTION, "EnableAudio", v);
	cfg_flush();
}

bool cfg_auto_extend()
{
	if (!config_has_user_value(user_config(), CFG_SECTION, "AutoExtend"))
		return true;
	return config_get_bool(user_config(), CFG_SECTION, "AutoExtend");
}
void cfg_set_auto_extend(bool v)
{
	config_set_bool(user_config(), CFG_SECTION, "AutoExtend", v);
	cfg_flush();
}

} // namespace capcast
