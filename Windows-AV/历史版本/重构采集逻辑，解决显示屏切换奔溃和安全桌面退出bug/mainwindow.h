#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QGraphicsPixmapItem>
#include <QThread>
#include <QComboBox>
#include <QImage>
#include <atomic>

// Qt Multimedia (Qt 6)
#include <QMediaDevices>
#include <QCameraDevice>
#include <QCamera>
#include <QMediaCaptureSession>
#include <QVideoSink>
#include <QVideoFrame>

class DxGiCapturer;
class VideoSourceWorker;
class ResizablePixmapItem; // 【新增1】添加前置声明

// ... CameraContext 结构体代码保持不变 ...
struct CameraContext {
    int itemId;
    QCamera* camera = nullptr;
    QMediaCaptureSession* session = nullptr;
    QVideoSink* sink = nullptr;

    QThread* workerThread = nullptr;
    VideoSourceWorker* worker = nullptr;

    // 【新增】原子标志位，确保后台处理不积压
    std::atomic<bool> isProcessing{false};
};

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    // ... 槽函数保持不变 ...
    void onFrameCaptured(QImage img);
    void onSnapshotCaptured(QImage img);
    void onLoadImage();
    void onAddCamera();
    void onOutputResult();
    void onMonitorChanged(int index);
    void onCameraFrameForUI(int itemId, QImage img);
    void onItemGeometryChanged(int id, QRect rect);

private:
    void setupUi();
    void startCaptureThread(int monitorIndex);
    void stopCaptureThread();
    void cleanupCameras();

private:
    QGraphicsScene *m_scene;
    QGraphicsView *m_view;
    QGraphicsPixmapItem *m_bgItem;
    QComboBox *m_comboMonitors;
    QComboBox *m_comboCameras;

    QThread* m_captureThread;
    DxGiCapturer* m_capturer;
    bool m_pendingRestart;
    int m_pendingMonitorIndex;

    int m_nextItemId = 1;

    QList<CameraContext*> m_cameras;

    // 【修改处】将类型从 QGraphicsPixmapItem* 改为 ResizablePixmapItem*
    QMap<int, ResizablePixmapItem*> m_cameraItems;
};
#endif
