/*
 * capcast-settings.hpp - CapCast 设置面板(工具菜单弹出)
 */
#pragma once

#include <QDialog>

class QComboBox;
class QCheckBox;
class QPushButton;
class QLabel;
class QSlider;
class QCloseEvent;

class CapCastSettings : public QDialog {
	Q_OBJECT

public:
	explicit CapCastSettings(QWidget *parent = nullptr);

	/* 从菜单调用: 显示/隐藏 切换 */
	void toggleShowHide();

	/* 刷新屏幕与音频设备列表(显示器/设备插拔后调用) */
	void refreshDevices();

private slots:
	void onStartClicked();
	void onStopClicked();
	void onVolumeChanged(int value);
	void onTrackChanged(int index);

private:
	void buildUi();
	void loadSettings();
	void saveSettings();
	void refreshStatus();
	void closeEvent(QCloseEvent *event) override; /* 关闭时自动保存 */

	QComboBox *displayCombo = nullptr;
	QComboBox *audioCombo = nullptr;
	QComboBox *trackCombo = nullptr;
	QComboBox *sourceCombo = nullptr;
	QSlider *volumeSlider = nullptr;
	QLabel *volumeLabel = nullptr;
	QCheckBox *autoStartCheck = nullptr;
	QPushButton *startBtn = nullptr;
	QPushButton *stopBtn = nullptr;
	QLabel *statusLabel = nullptr;
};
