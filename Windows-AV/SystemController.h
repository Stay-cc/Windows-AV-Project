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
    // 视频采集线程对象
    QThread* m_videoThread = nullptr;
    // 运行在 m_videoThread 中
    DxGiCapturer* m_capturer = nullptr;
    // 自身就是线程（继承 QThread）
    FFmpegRecorder* m_recorder = nullptr;
    // 麦克风采集线程
    WasapiCapturer* m_micCapturer = nullptr;
    // 系统声音采集线程
    WasapiCapturer* m_sysCapturer = nullptr;

    // 活动中的图层列表
    QList<CameraContext*> m_activeLayers;

    // 资源池：[DeviceId] -> [Running WinRTCamera Instance]
    // 预热的摄像头实例池
    QMap<QString, WinRTCamera*> m_cameraPool;
    // 保护以上两个容器
    QMutex m_poolMutex;

    // 图层 ID 自增计数器
    int m_nextItemId = 100;
    // 当前录制配置
    RecorderConfig m_config;
    // 每秒触发，更新录制时长
    QTimer* m_durationTimer;
    // 高精度计时器
    QElapsedTimer m_recordElapsed;
    // 当前选中的麦克风 ID
    QString m_currentMicId;
    // 当前选中的系统音频 ID
    QString m_currentSysId;
    // 麦克风混音开关
    bool m_isMicEnabled = false;
};

#endif // SYSTEMCONTROLLER_H
