#include "SystemController.h"
#include "EventBus.h"
#include <QDebug>
#include <QDateTime>
#include <QDir>
#include <QCoreApplication>

#include <windows.h>
#include <objbase.h>
#include <roapi.h>
#pragma comment(lib, "runtimeobject.lib")

SystemController::SystemController(QObject *parent) : QObject(parent)
{
    m_recorder = new FFmpegRecorder(nullptr);
    m_durationTimer = new QTimer(this);
    m_durationTimer->setInterval(1000);
    connect(m_durationTimer, &QTimer::timeout, this, &SystemController::onDurationTimer);
}

SystemController::~SystemController() {
    if (m_recorder) {
        m_recorder->stopRecord();
        m_recorder->wait();
        delete m_recorder;
        m_recorder = nullptr;
    }
    stopVideoThread();
    cleanupCameras();
    if (m_micCapturer) {
        m_micCapturer->stop();
        m_micCapturer->wait();
        delete m_micCapturer;
    }
    if (m_sysCapturer) {
        m_sysCapturer->stop();
        m_sysCapturer->wait();
        delete m_sysCapturer;
    }
}

void SystemController::initialize() {
    EventBus* bus = EventBus::instance();

    connect(bus, &EventBus::cmd_changeSource, this, &SystemController::onChangeSource);
    connect(bus, &EventBus::cmd_addCamera, this, &SystemController::onAddCamera);
    connect(bus, &EventBus::cmd_toggleRecording, this, &SystemController::onToggleRecording);
    connect(bus, &EventBus::cmd_updateConfig, this, &SystemController::onUpdateConfig);
    connect(bus, &EventBus::cmd_refreshDevices, this, &SystemController::onRefreshDevices);
    connect(bus, &EventBus::cmd_takeSnapshot, this, &SystemController::onTakeSnapshot);
    connect(bus, &EventBus::cmd_changeAudioDevice, this, &SystemController::onChangeAudioDevice);
    connect(bus, &EventBus::cmd_uiFrameProcessed, this, &SystemController::onUiFrameProcessed);
    connect(bus, &EventBus::cmd_overlayGeometryChanged, this, &SystemController::onOverlayGeometryChanged);
    connect(bus, &EventBus::cmd_toggleMicCapture, this, &SystemController::onToggleMicCapture);

    // 1. 启动视频线程 (涉及 WinRT，需要 MTA)
    startVideoThread();

    // 【关键修复】物理隔离：等待视频线程 D3D 初始化完毕，避免与 Audio 启动并发
    QThread::msleep(200);

    // 2. 刷新设备 (主线程，需要 STA)
    onRefreshDevices();

    // 3. 启动音频 (涉及 WASAPI，使用 STA)
    restartAudio(false, "");
    m_isMicEnabled = false;
}

void SystemController::startVideoThread() {
    if (m_videoThread) return;

    m_videoThread = new QThread(this);
    m_capturer = new DxGiCapturer();
    m_capturer->moveToThread(m_videoThread);

    connect(m_videoThread, &QThread::started, m_capturer, [this]() {
        // 视频线程使用 RoInitialize (WinRT/MTA)
        HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
        if (FAILED(hr)) CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        if (m_capturer->init()) {
            m_capturer->startCapture();
        } else {
            EventBus::instance()->fireError("Failed to initialize DXGI Capturer");
        }
        RoUninitialize();
    });

    connect(m_capturer, &DxGiCapturer::frameCaptured, this, &SystemController::onVideoFrameCaptured, Qt::QueuedConnection);
    connect(m_capturer, &DxGiCapturer::frameCapturedForPreview, EventBus::instance(), &EventBus::data_previewFrame, Qt::QueuedConnection);
    connect(m_capturer, &DxGiCapturer::snapshotCaptured, this, &SystemController::onSnapshotCaptured, Qt::QueuedConnection);
    connect(m_videoThread, &QThread::finished, m_capturer, &QObject::deleteLater);

    m_videoThread->start();
}

void SystemController::stopVideoThread() {
    if (m_capturer) {
        // 【关键修复】确保退出循环
        QMetaObject::invokeMethod(m_capturer, "stopCapture", Qt::BlockingQueuedConnection);
    }
    if (m_videoThread) {
        m_videoThread->quit();
        // 增加超时保护
        if (!m_videoThread->wait(3000)) {
            m_videoThread->terminate();
            m_videoThread->wait();
        }
        delete m_videoThread;
        m_videoThread = nullptr;
        m_capturer = nullptr;
    }
}

void SystemController::onChangeSource(int type, size_t id) {
    if (!m_capturer) return;
    CaptureTarget target;
    target.type = (type == 0) ? CaptureSourceType::Monitor : CaptureSourceType::Window;
    if (type == 0) target.monitorIndex = (int)id;
    else target.windowHandle = (HWND)id;
    m_capturer->setCaptureTarget(target);
}

void SystemController::onAddCamera(QVariant deviceVariant) {
    QCameraDevice device = deviceVariant.value<QCameraDevice>();
    if (device.isNull()) return;

    int itemId = m_nextItemId++;
    CameraContext* ctx = new CameraContext;
    ctx->itemId = itemId;
    ctx->camera = new QCamera(device);
    ctx->session = new QMediaCaptureSession;
    ctx->sink = new QVideoSink;
    ctx->session->setCamera(ctx->camera);
    ctx->session->setVideoOutput(ctx->sink);

    ctx->workerThread = new QThread(this);
    ctx->worker = new VideoSourceWorker(ctx->sink, itemId);
    ctx->worker->moveToThread(ctx->workerThread);

    connect(ctx->sink, &QVideoSink::videoFrameChanged, ctx->worker, &VideoSourceWorker::processFrame, Qt::QueuedConnection);

    connect(ctx->worker, &VideoSourceWorker::frameReadyForUI, [this, itemId](QImage img){
        EventBus::instance()->fireOverlayUpdate(itemId, img);
    });

    if (m_capturer) {
        connect(ctx->worker, &VideoSourceWorker::frameReadyForCompositor, m_capturer, &DxGiCapturer::updateOverlayImage, Qt::QueuedConnection);
        QMetaObject::invokeMethod(m_capturer, [=](){
                m_capturer->addOverlay(itemId, QImage(320, 240, QImage::Format_ARGB32), QRect(50, 50, 320, 240));
            }, Qt::QueuedConnection);
    }

    ctx->workerThread->start();
    ctx->camera->start();
    m_cameras.append(ctx);
}

void SystemController::onToggleRecording(bool start) {
    if (start) {
        if (m_config.savePath.isEmpty() || m_config.savePath.startsWith("Record_")) {
            m_config.savePath = "Record_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".mp4";
        }
        if (m_capturer) QMetaObject::invokeMethod(m_capturer, "startRecording", Qt::QueuedConnection);

        if (m_recorder->startRecord(m_config)) {
            m_recordElapsed.restart();
            m_durationTimer->start();
            EventBus::instance()->fireRecordingStateChanged(true, m_config.savePath);
        } else {
            EventBus::instance()->fireError("Failed to start recorder");
        }
    } else {
        m_recorder->stopRecord();
        if (m_capturer) QMetaObject::invokeMethod(m_capturer, "stopRecording", Qt::QueuedConnection);
        m_durationTimer->stop();
        EventBus::instance()->fireRecordingStateChanged(false, "");
        EventBus::instance()->fireDurationUpdate("00:00:00");
    }
}

void SystemController::onUpdateConfig(RecorderConfig cfg) { m_config = cfg; }

void SystemController::onRefreshDevices() {
    QList<QPair<QString, QVariant>> sources;
    QStringList monitors = DxGiCapturer::getMonitorNames();
    for (int i = 0; i < monitors.size(); ++i) sources.append({ "[Monitor] " + monitors[i], QVariant::fromValue<QList<QVariant>>({0, i}) });
    QMap<size_t, QString> windows = DxGiCapturer::getWindowList();
    for (auto it = windows.begin(); it != windows.end(); ++it) sources.append({ "[Window] " + it.value(), QVariant::fromValue<QList<QVariant>>({1, (unsigned long long)it.key()}) });
    EventBus::instance()->fireSourceListUpdated(sources);

    QStringList micNames, micIds;
    auto mics = WasapiCapturer::getAvailableDevices(WasapiCapturer::Microphone);
    micNames << "Default Microphone"; micIds << "";
    for(auto& d : mics) { micNames << d.name; micIds << d.id; }
    EventBus::instance()->fireAudioDeviceListUpdated(true, micNames, micIds);

    QStringList sysNames, sysIds;
    auto speakers = WasapiCapturer::getAvailableDevices(WasapiCapturer::SystemLoopback);
    sysNames << "Default System Audio"; sysIds << "";
    for(auto& d : speakers) { sysNames << d.name; sysIds << d.id; }
    EventBus::instance()->fireAudioDeviceListUpdated(false, sysNames, sysIds);
}

void SystemController::onTakeSnapshot() { if (m_capturer) m_capturer->requestSnapshot(); }

void SystemController::onChangeAudioDevice(bool isMic, QString deviceId) {
    if (isMic) {
        m_currentMicId = deviceId;
        if (m_isMicEnabled) restartAudio(true, deviceId);
    } else {
        m_currentSysId = deviceId;
        restartAudio(false, deviceId);
    }
}

void SystemController::onToggleMicCapture(bool active) {
    m_isMicEnabled = active;
    if (active) restartAudio(true, m_currentMicId);
    else {
        if (m_micCapturer) {
            m_micCapturer->stop();
            m_micCapturer->wait();
            delete m_micCapturer;
            m_micCapturer = nullptr;
        }
        EventBus::instance()->fireAudioLevel(true, 0.0f);
    }
}

void SystemController::onUiFrameProcessed() { if (m_capturer) QMetaObject::invokeMethod(m_capturer, "onUiFrameProcessed", Qt::QueuedConnection); }
void SystemController::onOverlayGeometryChanged(int id, QRect rect) { if (m_capturer) m_capturer->updateOverlayGeometry(id, rect); }
void SystemController::onVideoFrameCaptured(const QImage& img, qint64 timestamp) { m_recorder->pushVideoFrame(img, timestamp); }

void SystemController::onSnapshotCaptured(const QImage& img) {
    QString fileName = "Snapshot_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".png";
    if (img.save(fileName)) EventBus::instance()->fireSnapshotTaken(fileName);
    else EventBus::instance()->fireError("Failed to save snapshot");
}

void SystemController::onDurationTimer() {
    qint64 s = m_recordElapsed.elapsed() / 1000;
    QString timeStr = QString("%1:%2:%3").arg(s / 3600, 2, 10, QChar('0')).arg((s % 3600) / 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'));
    EventBus::instance()->fireDurationUpdate(timeStr);
}

void SystemController::restartAudio(bool isMic, const QString& deviceId) {
    WasapiCapturer** ptr = isMic ? &m_micCapturer : &m_sysCapturer;
    int channelId = isMic ? 1 : 0;
    WasapiCapturer::DeviceType type = isMic ? WasapiCapturer::Microphone : WasapiCapturer::SystemLoopback;

    if (*ptr) {
        (*ptr)->stop();
        (*ptr)->wait();
        delete *ptr;
        *ptr = nullptr;
    }

    *ptr = new WasapiCapturer(this);
    (*ptr)->setDevice(deviceId, type);

    connect(*ptr, &WasapiCapturer::volumeLevelChanged, this, [isMic](float level){
        EventBus::instance()->fireAudioLevel(isMic, level);
    });

    connect(*ptr, &WasapiCapturer::audioDataReady, m_recorder, [this, channelId](const QByteArray &data, int rate, int ch){
            if (m_recorder && m_recorder->isRunning()) {
                m_recorder->pushAudioData(channelId, data, rate, ch);
            }
        }, Qt::QueuedConnection);

    (*ptr)->start();
}

void SystemController::cleanupCameras() {
    for(auto ctx : m_cameras) {
        if (ctx->camera) ctx->camera->stop();
        if (ctx->workerThread) { ctx->workerThread->quit(); ctx->workerThread->wait(); }
        delete ctx->session; delete ctx->camera; delete ctx->sink; delete ctx->worker; delete ctx->workerThread; delete ctx;
    }
    m_cameras.clear();
}
