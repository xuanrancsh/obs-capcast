/*
 * plugin-main.cpp - CapCast 采集卡一键推流插件 主入口
 *
 * 参考 DistroAV / obs-plugintemplate 的模块组织方式:
 *   - obs_module_load 里注册工具菜单 + 事件回调 + 热键
 *   - 不注册 source/output/filter(纯前端工具型插件)
 */
#include "plugin-main.hpp"
#include "capcast-core.hpp"
#include "capcast-settings.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.h>

#include <QMainWindow>
#include <QAction>
#include <QApplication>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static CapCastSettings *g_settings = nullptr;
static obs_hotkey_id g_hotkey_id = OBS_INVALID_HOTKEY_ID;

/* ================= 工具菜单 ================= */

static void open_settings_menu()
{
	if (g_settings)
		g_settings->toggleShowHide();
}

/* ================= 一键推流(菜单/热键共用) ================= */

static void resolve_and_start()
{
	/* 解析目标屏幕(遵循配置: 手动 or 自动) */
	int screen_index = -1;
	if (capcast::cfg_display_mode() == QStringLiteral("manual")) {
		screen_index = capcast::cfg_display_index();
	} else {
		screen_index = capcast::pick_default_screen();
	}

	/* 解析目标音频设备 */
	QString audio_id;
	if (capcast::cfg_audio_mode() == QStringLiteral("manual")) {
		audio_id = capcast::cfg_audio_device_id();
	} else {
		audio_id = capcast::pick_default_audio().id;
	}

	const CapCastProjectorSource src =
		(capcast::cfg_source() == QStringLiteral("preview"))
			? CapCastProjectorSource::Preview
			: CapCastProjectorSource::Program;

	const QString err = capcast::start_output(screen_index, audio_id, src);
	if (!err.isEmpty()) {
		blog(LOG_WARNING, "[CapCast] start failed: %s",
		     qUtf8Printable(err));
	}
}

static void toggle_output()
{
	if (capcast::is_output_active()) {
		capcast::stop_output();
	} else {
		resolve_and_start();
	}
}

/* ================= 热键 ================= */

static void hotkey_callback(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		toggle_output();
}

/* ================= 事件回调 ================= */

static void frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		/* OBS 启动完成后, 按配置自动开始 */
		if (capcast::cfg_auto_start()) {
			blog(LOG_INFO, "[CapCast] auto start on launch");
			QMetaObject::invokeMethod(
				qApp, [] { resolve_and_start(); },
				Qt::QueuedConnection);
		}
	} else if (event == OBS_FRONTEND_EVENT_EXIT ||
		   event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN) {
		/* OBS 开始关闭: 立刻停止投屏与音频路由, 移除场景上的过滤器,
		 * 避免后续被 OBS 销毁时回调到我们已卸载的代码 */
		capcast::stop_output();
	}
}

/* ================= 模块入口 ================= */

bool obs_module_load(void)
{
	blog(LOG_INFO, "%s %s loaded", PLUGIN_DISPLAY_NAME, PLUGIN_VERSION);

	/* 默认配置(首次运行时写入) */
	if (capcast::cfg_source().isEmpty())
		capcast::cfg_set_source(QStringLiteral("program"));
	if (!capcast::cfg_auto_start())
		capcast::cfg_set_auto_start(false);

	/* 工具菜单: 设置面板 */
	QMainWindow *main_window =
		static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (main_window) {
		auto *menu_action = static_cast<QAction *>(
			obs_frontend_add_tools_menu_qaction(
				obs_module_text("CapCast.Menu.Settings")));
		menu_action->connect(menu_action, &QAction::triggered,
				     open_settings_menu);

		obs_frontend_push_ui_translation(obs_module_get_string);
		g_settings = new CapCastSettings(main_window);
		obs_frontend_pop_ui_translation();
	}

	/* 事件回调: 启动自动推流 / 退出清理 */
	obs_frontend_add_event_callback(frontend_event, nullptr);

	/* 全局热键: 一键开始/停止 */
	g_hotkey_id = obs_hotkey_register_frontend(
		"capcast.toggle_output",
		obs_module_text("CapCast.Hotkey.Toggle"), hotkey_callback,
		nullptr);

	blog(LOG_INFO, "[CapCast] plugin loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[CapCast] unloading...");

	/* 1. 先注销回调, 防止卸载后 OBS 再回调到已失效的代码 */
	obs_frontend_remove_event_callback(frontend_event, nullptr);

	/* 2. 注销热键 */
	if (g_hotkey_id != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(g_hotkey_id);
		g_hotkey_id = OBS_INVALID_HOTKEY_ID;
	}

	/* 3. 停止投屏与音频路由(移除场景上的过滤器 + 释放 WASAPI) */
	capcast::stop_output();

	/* 4. 同步销毁设置对话框。
	 *    注意: 不能用 deleteLater() —— 延迟删除会被推迟到插件卸载之后执行,
	 *    届时析构函数所在的代码已失效, 会调用无效地址导致 OBS 崩溃。
	 *    同步 delete 时代码仍然有效, 且会正确从父窗口(OBS 主窗口)的子列表中移除。 */
	if (g_settings) {
		delete g_settings;
		g_settings = nullptr;
	}

	blog(LOG_INFO, "[CapCast] plugin unloaded");
}
