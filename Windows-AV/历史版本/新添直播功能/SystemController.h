#ifndef SYSTEMCONTROLLER_H
#define SYSTEMCONTROLLER_H

#include <QObject>
#include <QThread>
#include <QTimer>
#include <QElapsedTimer>
#include <QMap>
#include <QVariant>
#include <QtConcurrent>
#include <QSet>
#include <QMutex>

#include "DxGiCapturer.h"
#include "WinRTCamera.h" // 替换 MfCamera
#include "WasapiCapturer.h"
#include "FFmpegRecorder.h"
#include "RecorderConfig.h"

// 摄像头上下文，关联图层ID和物理设备
struct CameraContext {
    int itemId;
    QString deviceId;
    WinRTCamera* camera = nullptr;
};

class SystemController : public QObject
{
    Q_OBJECT
public:
    explicit SystemController(QObject *parent = nullptr);
    ~SystemController();
    void initialize();

private slots:
    // 捕获器(D3D环境)准备好后，启动资源预热
    void onCapturerInitFinished(bool success);

    void onChangeSource(int type, size_t id);
    void onAddCamera(QVariant deviceVariant);
    void onToggleRecording(bool start);
    void onUpdateConfig(RecorderConfig cfg);
    void onRefreshDevices();
    void onTakeSnapshot();
    void onChangeAudioDevice(bool isMic, QString deviceId);
    void onToggleMicCapture(bool active);
    void onUiFrameProcessed();
    void onOverlayGeometryChanged(int id, QRect rect);
    void onVideoFrameCaptured(const QImage& img, qint64 timestamp);
    void onSnapshotCaptured(const QImage& img);
    void onDurationTimer();

private:
    void startVideoThread();
    void stopVideoThread();
    void restartAudio(bool isMic, const QString& deviceId);

    // 核心优化：预热所有摄像头
    void prewarmAllCameras();
    void cleanupCameraPool();

private:
    QThread* m_videoThread = nullptr;
    DxGiCapturer* m_capturer = nullptr;
    FFmpegRecorder* m_recorder = nullptr;
    WasapiCapturer* m_micCapturer = nullptr;
    WasapiCapturer* m_sysCapturer = nullptr;

    // 活动中的图层列表
    QList<CameraContext*> m_activeLayers;

    // 资源池：[DeviceId] -> [Running WinRTCamera Instance]
    // 这些实例在程序生命周期内一直存活并运行
    QMap<QString, WinRTCamera*> m_cameraPool;
    QMutex m_poolMutex;

    int m_nextItemId = 100;
    RecorderConfig m_config;
    QTimer* m_durationTimer;
    QElapsedTimer m_recordElapsed;
    QString m_currentMicId;
    QString m_currentSysId;
    bool m_isMicEnabled = false;
};

#endif // SYSTEMCONTROLLER_H
