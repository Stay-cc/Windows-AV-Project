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

class MainWindow : public FramelessWidget // 继承自无边框基类
{
    Q_OBJECT
public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    // UI Slots
    void onBtnRecordClicked();
    void onBtnSettingsClicked(); // 打开设置
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

    void onItemGeometryChanged(int id, QRect rect);

private:
    void setupUi();
    // [新增] 重写调整大小事件
    void resizeEvent(QResizeEvent *event) override;
    void setupTitleBar(QVBoxLayout* rootLayout);
    void applyWindowConfig(); // 应用从设置中读取的窗口配置

private:
    // 标题栏
    QWidget *m_titleBarWidget;
    QPushButton *m_btnMin;
    QPushButton *m_btnMax; // 最大化按钮
    QPushButton *m_btnClose;
    QLabel *m_lblTitle;

    // 录屏 UI 组件
    QGraphicsScene *m_scene;
    QGraphicsView *m_view;
    QGraphicsPixmapItem *m_bgItem;

    QComboBox *m_comboSources;
    QComboBox *m_comboCameras;
    QComboBox *m_comboMic;
    QComboBox *m_comboSysAudio;
    QCheckBox *m_chkMixMic;

    QProgressBar *m_progressMic;
    QProgressBar *m_progressSys;

    QPushButton *m_btnRecord;
    QPushButton *m_btnSettings;
    QLabel *m_lblDuration;

    // 逻辑状态
    RecorderConfig m_config;
    QMap<int, ResizablePixmapItem*> m_overlayItems;
    bool m_isRecording = false;
    int m_nextImageId = 1000;
};

#endif // MAINWINDOW_H
