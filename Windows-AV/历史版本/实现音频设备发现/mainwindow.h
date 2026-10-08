#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QGraphicsPixmapItem>
#include <QThread>
#include <QComboBox>
#include <QImage>
#include <QProgressBar> // 新增：用于显示音量
#include <QLabel>
#include <atomic>

// Qt Multimedia
#include <QMediaDevices>
#include <QCameraDevice>
#include <QCamera>
#include <QMediaCaptureSession>
#include <QVideoSink>
#include <QVideoFrame>

class DxGiCapturer;
class VideoSourceWorker;
class ResizablePixmapItem;
class WasapiCapturer; // 前置声明

// 相机上下文
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

private slots:
    void onFrameCaptured(QImage img);
    void onSnapshotCaptured(QImage img);
    void onLoadImage();
    void onAddCamera();
    void onOutputResult();

    // 视频源切换
    void onSourceChanged(int index);
    void onRefreshSources();

    // 音频源切换
    void onRefreshAudioDevices();
    void onMicChanged(int index);
    void onSystemAudioChanged(int index);

    // 音频数据处理
    void onAudioVolumeChanged(bool isMic, float level);

    void onCameraFrameForUI(int itemId, QImage img);
    void onItemGeometryChanged(int id, QRect rect);

private:
    void setupUi();
    void startCaptureThread();
    void stopCaptureThread();
    void cleanupCameras();
    void startAudioCapture(); // 启动音频
    void stopAudioCapture();  // 停止音频

private:
    QGraphicsScene *m_scene;
    QGraphicsView *m_view;
    QGraphicsPixmapItem *m_bgItem;
    QComboBox *m_comboSources;
    QComboBox *m_comboCameras;

    // 音频控件
    QComboBox *m_comboMic;
    QComboBox *m_comboSysAudio;
    QProgressBar *m_progressMic;    // 麦克风音量条
    QProgressBar *m_progressSys;    // 系统音量条

    QThread* m_captureThread;
    DxGiCapturer* m_capturer;

    // 音频采集线程对象
    WasapiCapturer* m_micCapturer = nullptr;
    WasapiCapturer* m_sysCapturer = nullptr;

    int m_nextItemId = 1;

    QList<CameraContext*> m_cameras;
    QMap<int, ResizablePixmapItem*> m_cameraItems;
};
#endif
