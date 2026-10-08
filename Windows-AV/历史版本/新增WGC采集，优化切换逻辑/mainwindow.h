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

    // 源切换
    void onSourceChanged(int index);
    void onRefreshSources();

    void onCameraFrameForUI(int itemId, QImage img);
    void onItemGeometryChanged(int id, QRect rect);

private:
    void setupUi();
    void startCaptureThread();
    void stopCaptureThread();
    void cleanupCameras();

private:
    QGraphicsScene *m_scene;
    QGraphicsView *m_view;
    QGraphicsPixmapItem *m_bgItem;
    QComboBox *m_comboSources; // 统一的选择框
    QComboBox *m_comboCameras;

    QThread* m_captureThread;
    DxGiCapturer* m_capturer;

    int m_nextItemId = 1;

    QList<CameraContext*> m_cameras;
    QMap<int, ResizablePixmapItem*> m_cameraItems;
};
#endif
