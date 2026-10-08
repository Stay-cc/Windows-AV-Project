#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "FramelessWidget.h"
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QGraphicsPixmapItem>
#include <QComboBox>
#include <QVBoxLayout>
#include <QProgressBar>
#include <QLabel>
#include <QPushButton>
#include <QCheckBox>
#include <QMap>
#include "RecorderConfig.h"
#include "EventBus.h"

class ResizablePixmapItem;

class MainWindow : public FramelessWidget
{
    Q_OBJECT
public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    // UI Slots
    void onBtnRecordClicked();
    void onBtnSettingsClicked();
    void onBtnRefreshClicked();
    void onBtnAddCamClicked();
    void onBtnSnapshotClicked();
    void onBtnLoadImageClicked();
    void onChkMixMicToggled(bool checked);

    void onSourceComboChanged(int index);
    void onMicComboChanged(int index);
    void onSysComboChanged(int index);

    // EventBus Slots
    void updatePreview(QImage img);
    void updateOverlay(int id, QImage img);
    void updateAudioLevel(bool isMic, float level);
    void onRecordingStateChanged(bool isRecording, QString msg);
    void onDurationUpdate(QString timeStr);
    void onSourceListUpdated(QList<QPair<QString, QVariant>> sources);
    void onAudioDeviceListUpdated(bool isMic, QStringList names, QStringList ids);
    void onSnapshotTaken(QString path);
    void onError(QString msg);

    // 【新增】处理摄像头添加事件，生成控制框
    void onCameraAdded(int id, QRect rect);

    void onItemGeometryChanged(int id, QRect rect);

private:
    void setupUi();
    void resizeEvent(QResizeEvent *event) override;
    void setupTitleBar(QVBoxLayout* rootLayout);
    void applyWindowConfig();

    void refreshCameras();

private:
    QWidget *m_titleBarWidget = nullptr;
    QPushButton *m_btnMin = nullptr;
    QPushButton *m_btnMax = nullptr;
    QPushButton *m_btnClose = nullptr;
    QLabel *m_lblTitle = nullptr;

    QGraphicsScene *m_scene = nullptr;
    QGraphicsView *m_view = nullptr;
    QGraphicsPixmapItem *m_bgItem = nullptr;

    QComboBox *m_comboSources = nullptr;
    QComboBox *m_comboCameras = nullptr;
    QComboBox *m_comboMic = nullptr;
    QComboBox *m_comboSysAudio = nullptr;
    QCheckBox *m_chkMixMic = nullptr;

    QProgressBar *m_progressMic = nullptr;
    QProgressBar *m_progressSys = nullptr;

    QPushButton *m_btnRecord = nullptr;
    QPushButton *m_btnSettings = nullptr;
    QLabel *m_lblDuration = nullptr;

    RecorderConfig m_config;
    QMap<int, ResizablePixmapItem*> m_overlayItems;
    bool m_isRecording = false;
    int m_nextImageId = 1000;

    // 【新增】记录上一次的场景/分辨率大小，用于相对位置计算
    QSize m_lastSceneSize;
};

#endif // MAINWINDOW_H
