#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QGraphicsPixmapItem>
#include <QThread>
#include <QComboBox>
#include <QImage>
#include <QProgressBar>
#include <QLabel>
#include <QPushButton>
#include <QCheckBox> // [新增]
#include <QTimer>
#include <QElapsedTimer>
#include <atomic>

#include <QMediaDevices>
#include <QCameraDevice>
#include <QCamera>
#include <QMediaCaptureSession>
#include <QVideoSink>
#include <QVideoFrame>

class DxGiCapturer;
class VideoSourceWorker;
class ResizablePixmapItem;
class WasapiCapturer;
class FFmpegRecorder;

struct CameraContext {
    int itemId;
    QCamera* camera = nullptr;
    QMediaCaptureSession* session = nullptr;
    QVideoSink* sink = nullptr;
    QThread* workerThread = nullptr;
    VideoSourceWorker* worker = nullptr;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

signals:
    void uiFrameProcessed();

private slots:
    void onFrameCaptured(QImage img);
    void onSnapshotCaptured(QImage img);
    void onLoadImage();
    void onAddCamera();
    void onOutputResult();

    void onSourceChanged(int index);
    void onRefreshSources();

    // 音频相关槽函数
    void onRefreshAudioDevices();
    void onMixMicToggled(bool checked); // [新增] 麦克风开关槽函数
    void onMicChanged(int index);
    void onSystemAudioChanged(int index);
    void onAudioVolumeChanged(bool isMic, float level);

    void onCameraFrameForUI(int itemId, QImage img);
    void onItemGeometryChanged(int id, QRect rect);

    void onToggleRecord();
    void onTimerUpdate();

private:
    void setupUi();
    void startCaptureThread();
    void stopCaptureThread();
    void cleanupCameras();
    void startAudioCapture();
    void stopAudioCapture();

    // 辅助重启某个音频采集器
    void restartMicCapturer();
    void restartSysCapturer();

private:
    QGraphicsScene *m_scene;
    QGraphicsView *m_view;
    QGraphicsPixmapItem *m_bgItem;
    QComboBox *m_comboSources;
    QComboBox *m_comboCameras;

    // 音频控制控件
    QComboBox *m_comboMic;
    QComboBox *m_comboSysAudio;
    QProgressBar *m_progressMic;
    QProgressBar *m_progressSys;
    QCheckBox *m_chkMixMic; // [新增]

    QPushButton *m_btnRecord;
    QLabel *m_lblDuration;

    QTimer *m_uiTimer;
    QElapsedTimer m_recordTimer;

    QThread *m_captureThread;
    DxGiCapturer *m_capturer;
    QList<CameraContext*> m_cameras;
    int m_nextItemId = 100;
    QMap<int, ResizablePixmapItem*> m_cameraItems;

    WasapiCapturer *m_micCapturer = nullptr;
    WasapiCapturer *m_sysCapturer = nullptr;
    FFmpegRecorder *m_recorder = nullptr;
};

#endif // MAINWINDOW_H
