/*
 * capcast-settings.hpp - CapCast 设置面板(工具菜单弹出)
 */
#pragma once

#include <QWidget>

class QComboBox;
class QLineEdit;
class QCheckBox;
class QPushButton;
class QLabel;

class CapCastSettings : public QWidget {
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

private:
	void buildUi();
	void loadSettings();
	void saveSettings();
	void refreshStatus();

	QComboBox *displayCombo = nullptr;
	QLineEdit *displayPatternEdit = nullptr;
	QComboBox *audioCombo = nullptr;
	QLineEdit *audioPatternEdit = nullptr;
	QComboBox *sourceCombo = nullptr;
	QCheckBox *autoExtendCheck = nullptr;
	QCheckBox *autoStartCheck = nullptr;
	QPushButton *startBtn = nullptr;
	QPushButton *stopBtn = nullptr;
	QLabel *statusLabel = nullptr;
};
