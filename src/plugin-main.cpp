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
#include <QPointer>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

/* 用 QPointer 保存: 设置对话框是 OBS 主窗口的子对象, 退出时可能被 OBS
 * 先一步销毁。QPointer 会在对象被销毁后自动置空, 避免我们持有悬垂指针
 * 并二次 delete(双重析构 -> 调用已释放对象的虚析构 -> 崩溃)。 */
static QPointer<CapCastSettings> g_settings;
static obs_hotkey_id g_hotkey_id = OBS_INVALID_HOTKEY_ID;

/* ================= 工具菜单 ================= */

static void open_settings_menu()
{
	if (!g_settings.isNull())
		g_settings->toggleShowHide();
}

/* ================= 一键推流(菜单/热键共用) ================= */

/* 按"启用"勾选分别启动: 勾了投屏就开投屏, 勾了音频映射就开音频映射,
 * 两项都没勾则不做任何操作(内部已处理, 这里只负责取错误提示)。 */
static void resolve_and_start()
{
	const QString err = capcast::start_enabled();
	if (!err.isEmpty()) {
		blog(LOG_WARNING, "[CapCast] start failed: %s",
		     qUtf8Printable(err));
	}
}

static void toggle_output()
{
	if (capcast::is_output_active()) {
		capcast::stop_all(); /* 停止全部正在运行的, 与勾选状态无关 */
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
		/* OBS 启动完成后, 仅当"OBS 启动时自动开始"打开才动作。
		 * 语义:
		 *   - 该总闸关闭 -> 插件完全不启动, 不做任何事;
		 *   - 该总闸开启 -> 按上次勾选的项目分别启动
		 *     (只勾了音频就只开音频, 只勾了投屏就只开投屏, 都勾就都开);
		 *   - 上次两项都没勾 -> start_enabled() 内部直接跳过, 不操作。 */
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
		capcast::stop_all();
	}
}

/* ================= 模块入口 ================= */

bool obs_module_load(void)
{
	blog(LOG_INFO, "%s %s loaded", PLUGIN_DISPLAY_NAME, PLUGIN_VERSION);

	/* 默认配置(首次运行时写入)。
	 * 注意: 不要再写 `if (!cfg_auto_start()) cfg_set_auto_start(false)`
	 * —— 那是恒等操作(为 false 时又写一次 false), 毫无意义。
	 * 功能勾选(EnableProjector/EnableAudio)默认 true, 由 core 内部按
	 * "从未设置过"处理, 这里不需要预写。 */
	if (capcast::cfg_source().isEmpty())
		capcast::cfg_set_source(QStringLiteral("program"));

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

	/* 3. 只停止音频路由并释放 WASAPI, 绝不触碰 Qt 控件。
	 *
	 *    这里若走 capcast::stop_all(), 它会去遍历
	 *    QApplication::topLevelWidgets() 并对投影窗口调用 close()。
	 *    卸载阶段 OBS 正在销毁主窗口与各投影窗口, close() 会让 Qt 把事件
	 *    派发到正在析构/已析构的对象上 —— 虚调用打到被释放对象的 vtable,
	 *    于是每次退出 OBS 都崩在 obs_module_unload:
	 *      capcast.dll!obs_module_unload -> qt6core -> qt6core -> <invalid>
	 *
	 *    投影窗口由 OBS 自己在退出时销毁; 正常停止(菜单/热键/OBS 退出事件)
	 *    仍会走 stop_all() 去关投影。 */
	capcast::stop_audio_only();

	/* 4. 设置对话框不在这里 delete。
	 *    它是 OBS 主窗口的子对象, 由 Qt 的父子所有权自动销毁, 不会泄漏;
	 *    而我们若在卸载阶段同步 delete, 一旦 OBS 已经先销毁过它, 就是
	 *    二次析构(悬垂指针 -> 崩溃)。g_settings 用 QPointer 持有, 被销毁
	 *    后自动置空, 这里清空引用即可。 */
	g_settings = nullptr;

	blog(LOG_INFO, "[CapCast] plugin unloaded");
}
