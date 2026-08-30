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

	autoStartCheck = new QCheckBox(
		obs_module_text("CapCast.Settings.AutoStart"), this);
	autoStartCheck->setToolTip(QObject::tr(
		"勾选后, OBS 启动完成时自动开始投屏和音频路由"));

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

	auto *audioBox = new QGroupBox(
		obs_module_text("CapCast.Settings.Group.Audio"), this);
	auto *audioForm = new QFormLayout(audioBox);
	audioForm->addRow(obs_module_text("CapCast.Settings.Audio.Target"),
			  audioCombo);
	audioForm->addRow(obs_module_text("CapCast.Settings.Audio.Volume"),
			  volumeRow);
	audioForm->addRow(obs_module_text("CapCast.Settings.Audio.Track"),
			  trackCombo);

	auto *outputBox = new QGroupBox(
		obs_module_text("CapCast.Settings.Group.Output"), this);
	auto *outputForm = new QFormLayout(outputBox);
	outputForm->addRow(obs_module_text("CapCast.Settings.Source"),
			   sourceCombo);
	outputForm->addRow(QString(), autoStartCheck);

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

	/* 让字段(下拉框)始终填满整行, 避免被挤压 */
	displayForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	audioForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	outputForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	/* 标签列宽度按内容自适应, 字段列占满剩余 */
	displayForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
	audioForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
	outputForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

	/* 强制最小尺寸, 防止 Qt 按 layout 算出的最小尺寸把窗口缩太窄 */
	setMinimumSize(460, 385);
	resize(520, 430);
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

	autoStartCheck->setChecked(capcast::cfg_auto_start());
}

void CapCastSettings::saveSettings()
{
	capcast::cfg_set_source(sourceCombo->currentData().toString());
	capcast::cfg_set_auto_start(autoStartCheck->isChecked());

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
	QString text;

	if (capcast::is_output_active()) {
		text = obs_module_text("CapCast.Status.Running");
	} else {
		text = obs_module_text("CapCast.Status.Idle");
	}

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
}

void CapCastSettings::onStartClicked()
{
	saveSettings();
	refreshDevices();

	/* 1. 解析目标屏幕 */
	int screen_index = displayCombo->currentData().toInt();
	if (screen_index < 0)
		screen_index = capcast::pick_default_screen();

	/* 2. 解析目标音频设备 */
	QString audio_id = audioCombo->currentData().toString();
	if (audio_id.isEmpty())
		audio_id = capcast::pick_default_audio().id;

	const CapCastProjectorSource src =
		(sourceCombo->currentData().toString() ==
		 QStringLiteral("preview"))
			? CapCastProjectorSource::Preview
			: CapCastProjectorSource::Program;

	const QString err = capcast::start_output(screen_index, audio_id, src);
	if (err.isEmpty()) {
		refreshStatus();
	} else {
		statusLabel->setText(
			obs_module_text("CapCast.Status.Error") +
			QStringLiteral(": ") + err);
	}
}

void CapCastSettings::onStopClicked()
{
	capcast::stop_output();
	refreshStatus();
}
