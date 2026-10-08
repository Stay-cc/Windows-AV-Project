#ifndef SYSTEMCONTROLLER_H
#define SYSTEMCONTROLLER_H

#include <QObject>
#include <QThread>
#include <QTimer>
#include <QElapsedTimer>
#include <QMap>
#include <QMediaDevices>
#include <QCameraDevice>
#include <QCamera>
#include <QMediaCaptureSession>
#include <QVideoSink>

#include "DxGiCapturer.h"
#include "WasapiCapturer.h"
#include "FFmpegRecorder.h"
#include "VideoSourceWorker.h"
#include "RecorderConfig.h"

struct CameraContext {
    int itemId;
    QCamera* camera = nullptr;
    QMediaCaptureSession* session = nullptr;
    QVideoSink* sink = nullptr;
    QThread* workerThread = nullptr;
    VideoSourceWorker* worker = nullptr;
};

class SystemController : public QObject
{
    Q_OBJECT
public:
    explicit SystemController(QObject *parent = nullptr);
    ~SystemController();

    void initialize();

private slots:
    void onChangeSource(int type, size_t id);
    void onAddCamera(QVariant deviceVariant);
    void onToggleRecording(bool start);
    void onUpdateConfig(RecorderConfig cfg);
    void onRefreshDevices();
    void onTakeSnapshot();
    void onChangeAudioDevice(bool isMic, QString deviceId);
    // [新增]
    void onToggleMicCapture(bool active);

    void onUiFrameProcessed();
    void onOverlayGeometryChanged(int id, QRect rect);

    // Internal
    void onVideoFrameCaptured(const QImage& img, qint64 timestamp);
    void onSnapshotCaptured(const QImage& img);
    void onDurationTimer();

private:
    void startVideoThread();
    void stopVideoThread();
    void restartAudio(bool isMic, const QString& deviceId);
    void cleanupCameras();

private:
    QThread* m_videoThread = nullptr;
    DxGiCapturer* m_capturer = nullptr;
    FFmpegRecorder* m_recorder = nullptr;

    WasapiCapturer* m_micCapturer = nullptr;
    WasapiCapturer* m_sysCapturer = nullptr;

    QList<CameraContext*> m_cameras;
    int m_nextItemId = 100;

    RecorderConfig m_config;
    QTimer* m_durationTimer;
    QElapsedTimer m_recordElapsed;

    // 记住当前的 ID，方便开关切换时恢复
    QString m_currentMicId;
    QString m_currentSysId;
    bool m_isMicEnabled = false; // [新增] 状态标记
};

#endif // SYSTEMCONTROLLER_H
