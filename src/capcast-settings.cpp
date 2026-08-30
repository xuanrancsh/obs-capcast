/*
 * capcast-settings.cpp - CapCast 设置面板实现
 */
#include "capcast-settings.hpp"
#include "capcast-core.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QComboBox>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QApplication>

namespace {

/* 把 Qt 屏幕列表填进下拉框, 返回当前选中的屏幕序号(-1 表示自动) */
int populate_screens(QComboBox *combo, int current_index)
{
	combo->clear();
	combo->addItem(QObject::tr("自动检测（按名称匹配）"), -1);

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
	combo->addItem(QObject::tr("自动检测（按名称匹配）"), QString());

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

CapCastSettings::CapCastSettings(QWidget *parent) : QWidget(parent)
{
	buildUi();
	loadSettings();
	refreshDevices();
	refreshStatus();
}

void CapCastSettings::buildUi()
{
	setWindowTitle(obs_module_text("CapCast.Settings.Title"));
	setAttribute(Qt::WA_DeleteOnClose, false);

	displayCombo = new QComboBox(this);
	displayPatternEdit = new QLineEdit(this);
	displayPatternEdit->setPlaceholderText(
		obs_module_text("CapCast.Settings.DisplayPattern.Placeholder"));

	audioCombo = new QComboBox(this);
	audioPatternEdit = new QLineEdit(this);
	audioPatternEdit->setPlaceholderText(
		obs_module_text("CapCast.Settings.AudioPattern.Placeholder"));

	sourceCombo = new QComboBox(this);
	sourceCombo->addItem(obs_module_text("CapCast.Settings.Source.Program"),
			     QStringLiteral("program"));
	sourceCombo->addItem(obs_module_text("CapCast.Settings.Source.Preview"),
			     QStringLiteral("preview"));

	autoExtendCheck = new QCheckBox(
		obs_module_text("CapCast.Settings.AutoExtend"), this);
	autoStartCheck = new QCheckBox(
		obs_module_text("CapCast.Settings.AutoStart"), this);

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
	displayForm->addRow(obs_module_text("CapCast.Settings.Display.Pattern"),
			    displayPatternEdit);

	auto *audioBox = new QGroupBox(
		obs_module_text("CapCast.Settings.Group.Audio"), this);
	auto *audioForm = new QFormLayout(audioBox);
	audioForm->addRow(obs_module_text("CapCast.Settings.Audio.Target"),
			  audioCombo);
	audioForm->addRow(obs_module_text("CapCast.Settings.Audio.Pattern"),
			  audioPatternEdit);

	auto *outputBox = new QGroupBox(
		obs_module_text("CapCast.Settings.Group.Output"), this);
	auto *outputForm = new QFormLayout(outputBox);
	outputForm->addRow(obs_module_text("CapCast.Settings.Source"),
			   sourceCombo);
	outputForm->addRow(QString(), autoExtendCheck);
	outputForm->addRow(QString(), autoStartCheck);

	auto *btnRow = new QHBoxLayout;
	btnRow->addWidget(startBtn);
	btnRow->addWidget(stopBtn);
	btnRow->addStretch(1);

	auto *mainLayout = new QVBoxLayout(this);
	mainLayout->addWidget(displayBox);
	mainLayout->addWidget(audioBox);
	mainLayout->addWidget(outputBox);
	mainLayout->addLayout(btnRow);
	mainLayout->addWidget(statusLabel);

	connect(startBtn, &QPushButton::clicked, this,
		&CapCastSettings::onStartClicked);
	connect(stopBtn, &QPushButton::clicked, this,
		&CapCastSettings::onStopClicked);

	resize(460, 380);
}

void CapCastSettings::toggleShowHide()
{
	if (isVisible()) {
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
	displayCombo->setCurrentIndex(-1);
	displayPatternEdit->setText(capcast::cfg_display_pattern());
	audioPatternEdit->setText(capcast::cfg_audio_pattern());

	const QString src = capcast::cfg_source();
	sourceCombo->setCurrentIndex(src == QStringLiteral("preview") ? 1 : 0);

	autoExtendCheck->setChecked(capcast::cfg_auto_extend());
	autoStartCheck->setChecked(capcast::cfg_auto_start());
}

void CapCastSettings::saveSettings()
{
	capcast::cfg_set_display_pattern(displayPatternEdit->text().trimmed());
	capcast::cfg_set_audio_pattern(audioPatternEdit->text().trimmed());
	capcast::cfg_set_source(sourceCombo->currentData().toString());
	capcast::cfg_set_auto_extend(autoExtendCheck->isChecked());
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
		const int found = capcast::find_screen_by_pattern(
			displayPatternEdit->text());
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
		const auto dev = capcast::find_audio_device_by_pattern(
			audioPatternEdit->text());
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

	int screen_index = -1;

	/* 1. 解析目标屏幕 */
	if (displayCombo->currentData().toInt() >= 0) {
		screen_index = displayCombo->currentData().toInt();
	} else {
		screen_index = capcast::find_screen_by_pattern(
			displayPatternEdit->text());
		/* 2. 自动模式下找不到 -> 按配置尝试点亮副屏(扩展显示) */
		if (screen_index < 0 && autoExtendCheck->isChecked()) {
			statusLabel->setText(
				obs_module_text("CapCast.Status.Extending"));
			if (capcast::ensure_display_extend()) {
				QApplication::processEvents();
				refreshDevices();
				screen_index = capcast::find_screen_by_pattern(
					displayPatternEdit->text());
			}
		}
	}

	/* 3. 解析目标音频设备 */
	QString audio_id;
	if (!audioCombo->currentData().toString().isEmpty()) {
		audio_id = audioCombo->currentData().toString();
	} else {
		const auto dev = capcast::find_audio_device_by_pattern(
			audioPatternEdit->text());
		audio_id = dev.id;
	}

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
