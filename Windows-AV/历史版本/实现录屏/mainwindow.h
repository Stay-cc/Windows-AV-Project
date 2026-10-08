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

    // 【关键修复】这里必须声明信号，否则 mainwindow.cpp 会报错 C2039/C2065
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

    void onRefreshAudioDevices();
    void onMicChanged(int index);
    void onSystemAudioChanged(int index);

    void onAudioVolumeChanged(bool isMic, float level);
    void onCameraFrameForUI(int itemId, QImage img);
    void onItemGeometryChanged(int id, QRect rect);

    // 录制按钮槽函数
    void onToggleRecord();

private:
    void setupUi();
    void startCaptureThread();
    void stopCaptureThread();
    void cleanupCameras();
    void startAudioCapture();
    void stopAudioCapture();

private:
    QGraphicsScene *m_scene;
    QGraphicsView *m_view;
    QGraphicsPixmapItem *m_bgItem;
    QComboBox *m_comboSources;
    QComboBox *m_comboCameras;

    QComboBox *m_comboMic;
    QComboBox *m_comboSysAudio;
    QProgressBar *m_progressMic;
    QProgressBar *m_progressSys;

    QPushButton *m_btnRecord;

    QThread* m_captureThread;
    DxGiCapturer* m_capturer;

    WasapiCapturer* m_micCapturer = nullptr;
    WasapiCapturer* m_sysCapturer = nullptr;

    FFmpegRecorder* m_recorder = nullptr;

    int m_nextItemId = 1;

    QList<CameraContext*> m_cameras;
    QMap<int, ResizablePixmapItem*> m_cameraItems;
};
#endif
