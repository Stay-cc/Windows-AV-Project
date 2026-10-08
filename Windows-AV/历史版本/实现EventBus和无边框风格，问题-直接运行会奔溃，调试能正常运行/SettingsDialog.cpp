#include "SettingsDialog.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QLabel>

SettingsDialog::SettingsDialog(RecorderConfig& config, QWidget *parent)
    : QDialog(parent), m_config(config) {
    setWindowTitle("Global Settings");
    resize(500, 550);
    // 稍微美化一下 Dialog
    setStyleSheet("QDialog { background-color: #333; color: white; } "
                  "QLabel, QCheckBox { color: white; } "
                  "QGroupBox { color: white; border: 1px solid #555; margin-top: 10px; } "
                  "QGroupBox::title { subcontrol-origin: margin; left: 10px; }");
    setupUi();
    loadFromConfig();
}

void SettingsDialog::setupUi() {
    QVBoxLayout* mainLayout = new QVBoxLayout(this);

    // 1. Window Behavior Group
    QGroupBox* grpWindow = new QGroupBox("Window Behavior");
    QVBoxLayout* winLayout = new QVBoxLayout(grpWindow);
    m_chkResizable = new QCheckBox("Enable Window Resizing (Unlock Size)");
    m_chkMaximizable = new QCheckBox("Enable Window Maximizing");
    winLayout->addWidget(m_chkResizable);
    winLayout->addWidget(m_chkMaximizable);
    mainLayout->addWidget(grpWindow);

    // 2. Video Settings
    QGroupBox* grpVideo = new QGroupBox("Recording Video Settings");
    QFormLayout* vidForm = new QFormLayout(grpVideo);
    m_comboEncoder = new QComboBox();
    m_comboEncoder->addItems({"h264_nvenc", "h264_qsv", "libx264"});
    vidForm->addRow("Encoder:", m_comboEncoder);

    QHBoxLayout* resLayout = new QHBoxLayout();
    m_spinWidth = new QSpinBox(); m_spinWidth->setRange(100, 7680);
    m_spinHeight = new QSpinBox(); m_spinHeight->setRange(100, 4320);
    resLayout->addWidget(m_spinWidth);
    resLayout->addWidget(new QLabel("x"));
    resLayout->addWidget(m_spinHeight);
    vidForm->addRow("Resolution:", resLayout);

    m_spinFps = new QSpinBox(); m_spinFps->setRange(1, 144);
    vidForm->addRow("FPS:", m_spinFps);
    m_spinBitrate = new QSpinBox(); m_spinBitrate->setRange(1, 100); m_spinBitrate->setSuffix(" Mbps");
    vidForm->addRow("Bitrate:", m_spinBitrate);
    mainLayout->addWidget(grpVideo);

    // 3. Audio Settings
    QGroupBox* grpAudio = new QGroupBox("Audio Processing");
    QFormLayout* audForm = new QFormLayout(grpAudio);
    m_spinFadeIn = new QSpinBox(); m_spinFadeIn->setRange(0, 5000); m_spinFadeIn->setSuffix(" ms");
    audForm->addRow("Fade In:", m_spinFadeIn);
    m_spinFadeOut = new QSpinBox(); m_spinFadeOut->setRange(0, 5000); m_spinFadeOut->setSuffix(" ms");
    audForm->addRow("Fade Out:", m_spinFadeOut);
    mainLayout->addWidget(grpAudio);

    // Buttons
    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, [this](){ saveToConfig(); accept(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttons);
}

void SettingsDialog::loadFromConfig() {
    // Window
    m_chkResizable->setChecked(m_config.windowResizable);
    m_chkMaximizable->setChecked(m_config.windowMaximizable);

    // Video & Audio
    m_comboEncoder->setCurrentText(m_config.encoderName);
    m_spinWidth->setValue(m_config.width);
    m_spinHeight->setValue(m_config.height);
    m_spinFps->setValue(m_config.fps);
    m_spinBitrate->setValue(m_config.videoBitrate / 1000000);
    m_spinFadeIn->setValue(m_config.fadeInDurationMs);
    m_spinFadeOut->setValue(m_config.fadeOutDurationMs);
}

void SettingsDialog::saveToConfig() {
    // Window
    m_config.windowResizable = m_chkResizable->isChecked();
    m_config.windowMaximizable = m_chkMaximizable->isChecked();

    // Video & Audio
    m_config.encoderName = m_comboEncoder->currentText();
    m_config.width = m_spinWidth->value();
    m_config.height = m_spinHeight->value();
    m_config.fps = m_spinFps->value();
    m_config.videoBitrate = m_spinBitrate->value() * 1000000;
    m_config.fadeInDurationMs = m_spinFadeIn->value();
    m_config.fadeOutDurationMs = m_spinFadeOut->value();
}

RecorderConfig SettingsDialog::getConfig() const {
    return m_config;
}
