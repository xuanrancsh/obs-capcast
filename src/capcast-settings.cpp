/*
 * capcast-settings.cpp - CapCast 设置面板实现
 */
#include "capcast-settings.hpp"
#include "capcast-core.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QComboBox>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QSlider>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QApplication>
#include <QCloseEvent>
#include <QToolTip>

namespace {

/* 把 Qt 屏幕列表填进下拉框, 返回当前选中的屏幕序号(-1 表示自动) */
int populate_screens(QComboBox *combo, int current_index)
{
	combo->clear();
	combo->addItem(QObject::tr("自动检测"), -1);

	int select_row = 0;
	const auto screens = capcast::enum_screens();
	for (const auto &sc : screens) {
		QString label = QStringLiteral("屏幕 %1  %2  %3")
					.arg(sc.index + 1)
					.arg(sc.name, sc.geometry);
		if (sc.isPrimary)
			label += QStringLiteral("  (主屏)");
		/* 系统缩放说明: geometry 已是物理分辨率, 这里补一句为什么
		 * OBS 会把它"看小了", 消除 4K 屏被显示成 1280x720 的误解 */
		if (sc.scale > 1.01) {
			label += QStringLiteral("  ") +
				 QString(obs_module_text(
					 "CapCast.Settings.Display.Scaled"))
					 .arg(qRound(sc.scale * 100.0))
					 .arg(sc.logical.width())
					 .arg(sc.logical.height());
		}
		combo->addItem(label, sc.index);
		if (sc.index == current_index)
			select_row = combo->count() - 1;
	}
	if (combo->count() > 0)
		combo->setCurrentIndex(select_row);
	return combo->currentData().toInt();
}

/* 把 WASAPI 输出端点列表填进下拉框, 返回当前选中的设备 ID */
QString populate_audio_devices(QComboBox *combo, const QString &current_id)
{
	combo->clear();
	combo->addItem(QObject::tr("自动检测"), QString());

	int select_row = 0;
	const auto devices = capcast::enum_audio_devices();
	for (const auto &d : devices) {
		combo->addItem(d.name, d.id);
		if (!current_id.isEmpty() && d.id == current_id)
			select_row = combo->count() - 1;
	}
	if (combo->count() > 0)
		combo->setCurrentIndex(select_row);
	return combo->currentData().toString();
}

} // namespace

CapCastSettings::CapCastSettings(QWidget *parent) : QDialog(parent)
{
	buildUi();
	loadSettings();
	refreshDevices();
	refreshStatus();
}

void CapCastSettings::buildUi()
{
	setWindowTitle(obs_module_text("CapCast.Settings.Title"));
	setWindowFlags(Qt::Window);
	setAttribute(Qt::WA_DeleteOnClose, false);

	displayCombo = new QComboBox(this);
	displayCombo->setToolTip(QObject::tr(
		"选择采集卡副屏。自动检测会优先选名称含采集卡关键字的副屏, 否则选第一个非主屏"));

	audioCombo = new QComboBox(this);
	audioCombo->setToolTip(QObject::tr(
		"选择采集卡音频端点(USB 声卡/HDMI 采集卡的音频输出)。自动检测会智能选择非音箱的采集设备"));

	/* 输出音量滑块: 0-200%, 默认 50%。只影响送到采集卡的声音 */
	volumeSlider = new QSlider(Qt::Horizontal, this);
	volumeSlider->setRange(0, 200);
	volumeSlider->setSingleStep(1);
	volumeSlider->setPageStep(5);
	volumeSlider->setTickPosition(QSlider::TicksBelow);
	volumeSlider->setTickInterval(25);
	volumeSlider->setToolTip(QObject::tr(
		"送到采集卡的音量。100% = OBS 主混音原始音量, 可往上放大或往下调小。只影响采集卡, 不改 OBS 混音器"));

	volumeLabel = new QLabel(QStringLiteral("50%"), this);
	volumeLabel->setMinimumWidth(52);
	volumeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

	auto *volumeRow = new QHBoxLayout;
	volumeRow->addWidget(volumeSlider, 1);
	volumeRow->addWidget(volumeLabel);

	/* 订阅音轨: 只把勾选了该音轨的源送到采集卡(默认音轨 1) */
	trackCombo = new QComboBox(this);
	for (int t = 1; t <= 6; ++t)
		trackCombo->addItem(QObject::tr("音轨 %1").arg(t), t);
	trackCombo->setToolTip(QObject::tr(
		"只把勾选了这条音轨的源送到采集卡。默认音轨 1 —— 若某个源在 OBS 混音器的高级音频属性里没有勾选音轨 1, 采集卡那边就听不到它。推流中切换立即生效"));

	sourceCombo = new QComboBox(this);
	sourceCombo->addItem(obs_module_text("CapCast.Settings.Source.Program"),
			     QStringLiteral("program"));
	sourceCombo->addItem(obs_module_text("CapCast.Settings.Source.Preview"),
			     QStringLiteral("preview"));

	/* 投屏 独立开关: [启用]勾选 + 立即启停按钮 */
	enableProjCheck = new QCheckBox(
		obs_module_text("CapCast.Settings.Enable.Projector"), this);
	enableProjCheck->setToolTip(QObject::tr(
		"勾选后, 点「一键推流」或 OBS 自动启动时才会开启投屏; 不勾选则完全不碰投屏"));

	projToggleBtn = new QPushButton(
		obs_module_text("CapCast.Settings.Toggle.Start"), this);
	projToggleBtn->setToolTip(QObject::tr(
		"立刻单独开启/关闭投屏, 不影响音频映射, 也不受上面勾选影响"));

	auto *projRow = new QHBoxLayout;
	projRow->addWidget(enableProjCheck);
	projRow->addStretch(1);
	projRow->addWidget(projToggleBtn);

	/* 音频映射 独立开关: [启用]勾选 + 立即启停按钮 */
	enableAudioCheck = new QCheckBox(
		obs_module_text("CapCast.Settings.Enable.Audio"), this);
	enableAudioCheck->setToolTip(QObject::tr(
		"勾选后, 点「一键推流」或 OBS 自动启动时才会开启音频映射; 不勾选则完全不碰音频"));

	audioToggleBtn = new QPushButton(
		obs_module_text("CapCast.Settings.Toggle.Start"), this);
	audioToggleBtn->setToolTip(QObject::tr(
		"立刻单独开启/关闭音频映射, 不影响投屏, 也不受上面勾选影响"));

	auto *audioRow = new QHBoxLayout;
	audioRow->addWidget(enableAudioCheck);
	audioRow->addStretch(1);
	audioRow->addWidget(audioToggleBtn);

	autoStartCheck = new QCheckBox(
		obs_module_text("CapCast.Settings.AutoStart"), this);
	autoStartCheck->setToolTip(QObject::tr(
		"总闸。勾选后 OBS 启动时才自动开始; 启动时按上面两项的勾选分别执行(上次只勾了音频就只开音频)。不勾选则 OBS 启动后插件完全不动"));

	autoExtendCheck = new QCheckBox(
		obs_module_text("CapCast.Settings.AutoExtend"), this);
	autoExtendCheck->setToolTip(QObject::tr(
		"找不到采集卡副屏时, 自动把 Windows 显示模式设为「扩展」并重试一次"));

	startBtn = new QPushButton(
		obs_module_text("CapCast.Settings.Start"), this);
	stopBtn = new QPushButton(obs_module_text("CapCast.Settings.Stop"), this);
	statusLabel = new QLabel(this);
	statusLabel->setWordWrap(true);

	auto *displayBox = new QGroupBox(
		obs_module_text("CapCast.Settings.Group.Display"), this);
	auto *displayForm = new QFormLayout(displayBox);
	displayForm->addRow(obs_module_text("CapCast.Settings.Display.Target"),
			    displayCombo);
	displayForm->addRow(QString(), projRow);

	auto *audioBox = new QGroupBox(
		obs_module_text("CapCast.Settings.Group.Audio"), this);
	auto *audioForm = new QFormLayout(audioBox);
	audioForm->addRow(obs_module_text("CapCast.Settings.Audio.Target"),
			  audioCombo);
	audioForm->addRow(obs_module_text("CapCast.Settings.Audio.Volume"),
			  volumeRow);
	audioForm->addRow(obs_module_text("CapCast.Settings.Audio.Track"),
			  trackCombo);
	audioForm->addRow(QString(), audioRow);

	auto *outputBox = new QGroupBox(
		obs_module_text("CapCast.Settings.Group.Output"), this);
	auto *outputForm = new QFormLayout(outputBox);
	outputForm->addRow(obs_module_text("CapCast.Settings.Source"),
			   sourceCombo);
	outputForm->addRow(QString(), autoStartCheck);
	outputForm->addRow(QString(), autoExtendCheck);

	auto *btnRow = new QHBoxLayout;
	btnRow->addWidget(startBtn);
	btnRow->addWidget(stopBtn);
	btnRow->addStretch(1);

	auto *mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(12, 12, 12, 12);
	mainLayout->setSpacing(8);
	mainLayout->addWidget(displayBox);
	mainLayout->addWidget(audioBox);
	mainLayout->addWidget(outputBox);
	mainLayout->addLayout(btnRow);
	mainLayout->addWidget(statusLabel);

	connect(startBtn, &QPushButton::clicked, this,
		&CapCastSettings::onStartClicked);
	connect(stopBtn, &QPushButton::clicked, this,
		&CapCastSettings::onStopClicked);
	connect(volumeSlider, &QSlider::valueChanged, this,
		&CapCastSettings::onVolumeChanged);
	connect(trackCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
		this, &CapCastSettings::onTrackChanged);
	connect(projToggleBtn, &QPushButton::clicked, this,
		&CapCastSettings::onProjectorToggleClicked);
	connect(audioToggleBtn, &QPushButton::clicked, this,
		&CapCastSettings::onAudioToggleClicked);

	/* 让字段(下拉框)始终填满整行, 避免被挤压 */
	displayForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	audioForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	outputForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	/* 标签列宽度按内容自适应, 字段列占满剩余 */
	displayForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
	audioForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
	outputForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

	/* 强制最小尺寸, 防止 Qt 按 layout 算出的最小尺寸把窗口缩太窄
	 * (新增了两行独立开关, 高度相应加大) */
	setMinimumSize(460, 470);
	resize(520, 520);
}

void CapCastSettings::closeEvent(QCloseEvent *event)
{
	/* 关闭窗口时保存当前设置, 下次打开保持 */
	saveSettings();
	event->accept();
}

void CapCastSettings::toggleShowHide()
{
	if (isVisible()) {
		saveSettings();
		hide();
	} else {
		refreshDevices();
		refreshStatus();
		show();
		raise();
		activateWindow();
	}
}

void CapCastSettings::refreshDevices()
{
	/* 记住当前选择, 刷新后尽量恢复 */
	const int cur_screen = displayCombo->currentData().toInt();
	populate_screens(displayCombo, cur_screen);
	const QString cur_audio = audioCombo->currentData().toString();
	populate_audio_devices(audioCombo, cur_audio);
}

void CapCastSettings::loadSettings()
{
	/* 显示目标: 手动 index 或自动 */
	if (capcast::cfg_display_mode() == QStringLiteral("manual")) {
		populate_screens(displayCombo, capcast::cfg_display_index());
	} else {
		populate_screens(displayCombo, -1);
	}

	/* 音频目标: 手动设备 id 或自动 */
	if (capcast::cfg_audio_mode() == QStringLiteral("manual")) {
		populate_audio_devices(audioCombo,
				       capcast::cfg_audio_device_id());
	} else {
		populate_audio_devices(audioCombo, QString());
	}

	const QString src = capcast::cfg_source();
	sourceCombo->setCurrentIndex(src == QStringLiteral("preview") ? 1 : 0);

	/* 输出音量(默认 50%) */
	const int vol = (int)(capcast::cfg_audio_volume() + 0.5);
	volumeSlider->setValue(vol);
	volumeLabel->setText(QStringLiteral("%1%").arg(vol));

	/* 订阅音轨(默认 1) */
	const int track = capcast::cfg_audio_track();
	const int track_row = trackCombo->findData(track);
	trackCombo->setCurrentIndex(track_row >= 0 ? track_row : 0);

	/* 功能勾选: 决定「一键推流」与 OBS 自动启动各自执行哪些项 */
	enableProjCheck->setChecked(capcast::cfg_enable_projector());
	enableAudioCheck->setChecked(capcast::cfg_enable_audio());

	autoStartCheck->setChecked(capcast::cfg_auto_start());
	autoExtendCheck->setChecked(capcast::cfg_auto_extend());
}

void CapCastSettings::saveSettings()
{
	capcast::cfg_set_source(sourceCombo->currentData().toString());
	capcast::cfg_set_auto_start(autoStartCheck->isChecked());

	/* 功能勾选(会持久化, 下次 OBS 启动按这个清单自动执行) */
	capcast::cfg_set_enable_projector(enableProjCheck->isChecked());
	capcast::cfg_set_enable_audio(enableAudioCheck->isChecked());
	capcast::cfg_set_auto_extend(autoExtendCheck->isChecked());

	/* 显示目标 */
	const int screen_idx = displayCombo->currentData().toInt();
	if (screen_idx >= 0) {
		capcast::cfg_set_display_mode(QStringLiteral("manual"));
		capcast::cfg_set_display_index(screen_idx);
	} else {
		capcast::cfg_set_display_mode(QStringLiteral("auto"));
	}

	/* 音频目标 */
	const QString dev_id = audioCombo->currentData().toString();
	if (!dev_id.isEmpty()) {
		capcast::cfg_set_audio_mode(QStringLiteral("manual"));
		capcast::cfg_set_audio_device(audioCombo->currentText(), dev_id);
	} else {
		capcast::cfg_set_audio_mode(QStringLiteral("auto"));
	}

	/* 输出音量(运行时已即时生效, 这里只负责落盘) */
	capcast::cfg_set_audio_volume((double)volumeSlider->value());

	/* 订阅音轨(运行时已即时切换, 这里只负责落盘) */
	capcast::cfg_set_audio_track(trackCombo->currentData().toInt());
}

void CapCastSettings::onVolumeChanged(int value)
{
	volumeLabel->setText(QStringLiteral("%1%").arg(value));

	/* 推流中拖动即刻改变送到采集卡的音量, 无需重启路由;
	 * 未推流时也会写入运行时值, 下次开始即生效 */
	capcast::set_output_volume((double)value);
}

void CapCastSettings::onTrackChanged(int)
{
	const int track = trackCombo->currentData().toInt();

	/* 推流中切换立即重新订阅; 未推流时只记录, 下次开始生效 */
	capcast::apply_audio_track(track);
}

void CapCastSettings::refreshStatus()
{
	/* 两项功能各自的状态, 互不影响 */
	QString text =
		QStringLiteral("%1: %2\n%3: %4")
			.arg(obs_module_text("CapCast.Status.Projector"),
			     capcast::is_projector_active()
				     ? obs_module_text("CapCast.Status.On")
				     : obs_module_text("CapCast.Status.Off"),
			     obs_module_text("CapCast.Status.AudioMapping"),
			     capcast::is_audio_active()
				     ? obs_module_text("CapCast.Status.On")
				     : obs_module_text("CapCast.Status.Off"));

	/* 显示识别结果 */
	const int screen_idx = displayCombo->currentData().toInt();
	if (screen_idx >= 0) {
		const auto screens = capcast::enum_screens();
		for (const auto &sc : screens) {
			if (sc.index == screen_idx) {
				text += QStringLiteral("\n[%1] %2 %3")
						.arg(obs_module_text("CapCast.Status.Display"),
						     sc.name, sc.geometry);
				break;
			}
		}
	} else {
		const int found = capcast::pick_default_screen();
		text += QStringLiteral("\n[%1] %2")
				.arg(obs_module_text("CapCast.Status.Display"),
				     found >= 0
					     ? obs_module_text("CapCast.Status.Display.Found")
					     : obs_module_text("CapCast.Status.Display.NotFound"));
	}

	/* 音频识别结果 */
	const QString dev_id = audioCombo->currentData().toString();
	if (!dev_id.isEmpty()) {
		text += QStringLiteral("\n[%1] %2")
				.arg(obs_module_text("CapCast.Status.Audio"),
				     audioCombo->currentText());
	} else {
		const auto dev = capcast::pick_default_audio();
		text += QStringLiteral("\n[%1] %2")
				.arg(obs_module_text("CapCast.Status.Audio"),
				     dev.name.isEmpty()
					     ? obs_module_text("CapCast.Status.Audio.NotFound")
					     : dev.name);
	}

	statusLabel->setText(text);
	refreshToggleButtons();
}

void CapCastSettings::onStartClicked()
{
	saveSettings();
	refreshDevices();

	/* 按勾选分别执行: 勾了投屏就开投屏, 勾了音频映射就开音频映射,
	 * 两项都没勾则不做任何操作。 */
	const QString err = capcast::start_enabled();
	if (err.isEmpty()) {
		refreshStatus();
	} else {
		statusLabel->setText(obs_module_text("CapCast.Status.Error") +
				     QStringLiteral(": ") + err);
		refreshToggleButtons();
	}
}

void CapCastSettings::onStopClicked()
{
	/* 停止全部正在运行的, 与勾选状态无关 */
	capcast::stop_all();
	refreshStatus();
}

/* 让两个独立按钮的文字跟随各自运行状态: 运行中显示"停止", 否则"启动" */
void CapCastSettings::refreshToggleButtons()
{
	projToggleBtn->setText(
		capcast::is_projector_active()
			? obs_module_text("CapCast.Settings.Toggle.Stop")
			: obs_module_text("CapCast.Settings.Toggle.Start"));
	audioToggleBtn->setText(
		capcast::is_audio_active()
			? obs_module_text("CapCast.Settings.Toggle.Stop")
			: obs_module_text("CapCast.Settings.Toggle.Start"));
}

void CapCastSettings::onProjectorToggleClicked()
{
	/* 先把下拉框当前选择落盘, 让这次启动立刻用上新参数 */
	saveSettings();

	if (capcast::is_projector_active()) {
		capcast::stop_projector();
	} else {
		int screen_index = displayCombo->currentData().toInt();
		if (screen_index < 0)
			screen_index = capcast::pick_default_screen();

		const CapCastProjectorSource src =
			(sourceCombo->currentData().toString() ==
			 QStringLiteral("preview"))
				? CapCastProjectorSource::Preview
				: CapCastProjectorSource::Program;

		const QString err = capcast::start_projector(screen_index, src);
		if (!err.isEmpty()) {
			statusLabel->setText(
				obs_module_text("CapCast.Status.Error") +
				QStringLiteral(": ") + err);
			refreshToggleButtons();
			return;
		}
	}
	refreshStatus();
}

void CapCastSettings::onAudioToggleClicked()
{
	saveSettings();

	if (capcast::is_audio_active()) {
		capcast::stop_audio_mapping();
	} else {
		QString audio_id = audioCombo->currentData().toString();
		if (audio_id.isEmpty())
			audio_id = capcast::pick_default_audio().id;

		const QString err = capcast::start_audio_mapping(audio_id);
		if (!err.isEmpty()) {
			statusLabel->setText(
				obs_module_text("CapCast.Status.Error") +
				QStringLiteral(": ") + err);
			refreshToggleButtons();
			return;
		}
	}
	refreshStatus();
}
