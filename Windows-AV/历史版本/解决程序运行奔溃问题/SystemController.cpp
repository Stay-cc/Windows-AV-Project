#include "SystemController.h"
#include "EventBus.h"
#include "Logger.h"
#include <QDebug>
#include <QDateTime>
#include <QMediaDevices>
#include <QCameraDevice>
#include <roapi.h>

SystemController::SystemController(QObject *parent) : QObject(parent) {
    m_recorder = new FFmpegRecorder(nullptr);
    m_durationTimer = new QTimer(this);
    m_durationTimer->setInterval(1000);
    connect(m_durationTimer, &QTimer::timeout, this, &SystemController::onDurationTimer);
    LOG_INFO("SystemController Created.");
}

SystemController::~SystemController() {
    LOG_INFO("SystemController Destroying...");
    if (m_recorder) { m_recorder->stopRecord(); m_recorder->wait(); delete m_recorder; m_recorder = nullptr; }

    stopVideoThread();
    cleanupCameraPool();

    if (m_micCapturer) { m_micCapturer->stop(); m_micCapturer->wait(); delete m_micCapturer; }
    if (m_sysCapturer) { m_sysCapturer->stop(); m_sysCapturer->wait(); delete m_sysCapturer; }
    LOG_INFO("SystemController Destroyed.");
}

void SystemController::initialize() {
    LOG_INFO("SystemController::initialize() Start");
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

    startVideoThread();

    LOG_INFO("SystemController::initialize() Return (Waiting for VideoThread signal)");
}

void SystemController::startVideoThread() {
    if (m_videoThread) return;
    LOG_INFO("Starting Video Thread...");
    m_videoThread = new QThread(this);
    m_capturer = new DxGiCapturer();
    m_capturer->moveToThread(m_videoThread);

    connect(m_videoThread, &QThread::started, m_capturer, [this]() {
        LOG_INFO("[VideoThread] Thread Started. Initializing COM...");
        HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
        if (FAILED(hr)) {
            LOG_WARN(QString("[VideoThread] RoInitialize Failed HR=%1, trying CoInit MTA").arg(hr));
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        }

        LOG_INFO("[VideoThread] Initializing Capturer...");
        if (m_capturer->init()) {
            LOG_INFO("[VideoThread] Capturer Init Success. Starting Capture...");
            m_capturer->startCapture();
        } else {
            LOG_ERROR("[VideoThread] Capturer Init Failed.");
            EventBus::instance()->fireError("Failed to initialize DXGI Capturer");
        }
    });

    connect(m_capturer, &DxGiCapturer::initFinished, this, &SystemController::onCapturerInitFinished, Qt::QueuedConnection);
    connect(m_capturer, &DxGiCapturer::frameCaptured, this, &SystemController::onVideoFrameCaptured, Qt::QueuedConnection);
    connect(m_capturer, &DxGiCapturer::frameCapturedForPreview, EventBus::instance(), &EventBus::data_previewFrame, Qt::QueuedConnection);
    connect(m_capturer, &DxGiCapturer::snapshotCaptured, this, &SystemController::onSnapshotCaptured, Qt::QueuedConnection);
    connect(m_videoThread, &QThread::finished, m_capturer, &QObject::deleteLater);
    connect(m_videoThread, &QThread::finished, this, [](){ RoUninitialize(); });

    m_videoThread->start();
}

void SystemController::stopVideoThread() {
    if (m_capturer) QMetaObject::invokeMethod(m_capturer, "stopCapture", Qt::BlockingQueuedConnection);
    if (m_videoThread) {
        m_videoThread->quit();
        m_videoThread->wait(3000);
        delete m_videoThread;
        m_videoThread = nullptr;
        m_capturer = nullptr;
    }
}

void SystemController::onCapturerInitFinished(bool success) {
    if (success) {
        LOG_INFO("[MainThread] GPU Init Done. Safe to init Audio & Devices now.");
        onRefreshDevices();
        restartAudio(false, "");
        prewarmAllCameras();
    } else {
        LOG_ERROR("[MainThread] GPU Init Aborted. Audio/Devices will not start.");
    }
}

void SystemController::prewarmAllCameras() {
    if (!m_capturer) return;

    QMetaObject::invokeMethod(m_capturer, [this]() {
        const auto cameras = QMediaDevices::videoInputs();
        ID3D11Device* rawDevice = m_capturer->getD3DDevice();
        if (!rawDevice) return;

        QMutexLocker locker(&m_poolMutex);

        for (const auto& info : cameras) {
            QString deviceId = QString::fromUtf8(info.id());
            QString name = info.description();
            if (m_cameraPool.contains(deviceId)) continue;

            LOG_INFO("Pre-warming Camera: " + name);
            WinRTCamera* cam = new WinRTCamera();

            if (cam->init(rawDevice)) {
                if (cam->start(deviceId)) {
                    m_cameraPool.insert(deviceId, cam);
                } else {
                    delete cam;
                }
            } else {
                delete cam;
            }
        }
    });
}

void SystemController::cleanupCameraPool() {
    if (m_videoThread && m_videoThread->isRunning()) {
        QMetaObject::invokeMethod(m_videoThread, [this]() {
                QMutexLocker locker(&m_poolMutex);
                for (auto cam : m_cameraPool) {
                    cam->stop();
                    delete cam;
                }
                m_cameraPool.clear();
            }, Qt::BlockingQueuedConnection);
    } else {
        QMutexLocker locker(&m_poolMutex);
        for (auto cam : m_cameraPool) delete cam;
        m_cameraPool.clear();
    }

    for (auto ctx : m_activeLayers) delete ctx;
    m_activeLayers.clear();
}

void SystemController::onAddCamera(QVariant deviceVariant) {
    QCameraDevice info = deviceVariant.value<QCameraDevice>();
    QString deviceId = QString::fromUtf8(info.id());

    // 防止重复添加
    for (auto ctx : m_activeLayers) {
        if (ctx->deviceId == deviceId) return;
    }

    WinRTCamera* targetCam = nullptr;
    {
        QMutexLocker locker(&m_poolMutex);
        if (m_cameraPool.contains(deviceId)) {
            targetCam = m_cameraPool.value(deviceId);
        }
    }

    // 默认显示位置
    QRect defaultRect(50, 50, 320, 240);

    if (targetCam) {
        LOG_INFO("Using Pre-warmed Camera.");
        int id = m_nextItemId++;
        m_capturer->addCameraOverlay(id, targetCam, defaultRect);

        CameraContext* ctx = new CameraContext;
        ctx->itemId = id; ctx->deviceId = deviceId; ctx->camera = targetCam;
        m_activeLayers.append(ctx);

        // 【核心修改】通知 UI 创建可交互的控制框
        EventBus::instance()->fireCameraAdded(id, defaultRect);

    } else if (m_capturer) {
        LOG_INFO("Cold starting camera...");
        QMetaObject::invokeMethod(m_capturer, [=]() {
            ID3D11Device* device = m_capturer->getD3DDevice();
            WinRTCamera* cam = new WinRTCamera();
            if (cam->init(device) && cam->start(deviceId)) {
                { QMutexLocker l(&m_poolMutex); m_cameraPool.insert(deviceId, cam); }
                int id = m_nextItemId++;
                m_capturer->addCameraOverlay(id, cam, defaultRect);

                QMetaObject::invokeMethod(this, [=]() {
                    CameraContext* ctx = new CameraContext;
                    ctx->itemId = id; ctx->deviceId = deviceId; ctx->camera = cam;
                    m_activeLayers.append(ctx);

                    // 【核心修改】通知 UI 创建可交互的控制框
                    EventBus::instance()->fireCameraAdded(id, defaultRect);
                });
            } else {
                delete cam;
                EventBus::instance()->fireError("Failed to open camera");
            }
        });
    }
}

void SystemController::onRefreshDevices() {
    QStringList monitors = DxGiCapturer::getMonitorNames();
    QList<QPair<QString, QVariant>> srcList;
    for(int i=0; i<monitors.size(); ++i) {
        srcList.append({ "[Monitor] "+monitors[i], QVariant::fromValue<QList<QVariant>>({0, i}) });
    }

    QMap<size_t, QString> windows = DxGiCapturer::getWindowList();
    QMapIterator<size_t, QString> i(windows);
    while (i.hasNext()) {
        i.next();
        srcList.append({ "[Window] " + i.value(), QVariant::fromValue<QList<QVariant>>({1, (qulonglong)i.key()}) });
    }

    EventBus::instance()->fireSourceListUpdated(srcList);

    QList<AudioDeviceInfo> mics = WasapiCapturer::getAvailableDevices(WasapiCapturer::Microphone);
    QStringList micNames, micIds;
    for(auto &d : mics) { micNames << d.name; micIds << d.id; }
    EventBus::instance()->fireAudioDeviceListUpdated(true, micNames, micIds);

    QList<AudioDeviceInfo> speakers = WasapiCapturer::getAvailableDevices(WasapiCapturer::SystemLoopback);
    QStringList sysNames, sysIds;
    for(auto &d : speakers) { sysNames << d.name; sysIds << d.id; }
    EventBus::instance()->fireAudioDeviceListUpdated(false, sysNames, sysIds);
}

void SystemController::onChangeSource(int type, size_t id) {
    if (!m_capturer) return;
    CaptureTarget target; target.type = (type == 0) ? CaptureSourceType::Monitor : CaptureSourceType::Window;
    if (type == 0) target.monitorIndex = (int)id; else target.windowHandle = (HWND)id;
    m_capturer->setCaptureTarget(target);
}
void SystemController::onToggleRecording(bool start) {
    if (start) {
        if (m_config.savePath.isEmpty()) m_config.savePath = "Rec_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".mp4";
        QMetaObject::invokeMethod(m_capturer, "startRecording", Qt::QueuedConnection);
        if (m_recorder->startRecord(m_config)) { m_recordElapsed.restart(); m_durationTimer->start(); EventBus::instance()->fireRecordingStateChanged(true, m_config.savePath); }
    } else {
        m_recorder->stopRecord(); QMetaObject::invokeMethod(m_capturer, "stopRecording", Qt::QueuedConnection);
        m_durationTimer->stop(); EventBus::instance()->fireRecordingStateChanged(false, ""); EventBus::instance()->fireDurationUpdate("00:00:00");
    }
}
void SystemController::onUpdateConfig(RecorderConfig cfg) { m_config = cfg; }
void SystemController::onTakeSnapshot() { if(m_capturer) m_capturer->requestSnapshot(); }
void SystemController::onChangeAudioDevice(bool isMic, QString deviceId) { if(isMic) { m_currentMicId=deviceId; if(m_isMicEnabled) restartAudio(true, deviceId); } else { m_currentSysId=deviceId; restartAudio(false, deviceId); } }
void SystemController::onToggleMicCapture(bool active) { m_isMicEnabled=active; if(active) restartAudio(true, m_currentMicId); else { if(m_micCapturer) {m_micCapturer->stop(); delete m_micCapturer; m_micCapturer=nullptr;} EventBus::instance()->fireAudioLevel(true, 0); } }
void SystemController::onUiFrameProcessed() { if(m_capturer) QMetaObject::invokeMethod(m_capturer, "onUiFrameProcessed", Qt::QueuedConnection); }
void SystemController::onOverlayGeometryChanged(int id, QRect rect) { if(m_capturer) m_capturer->updateOverlayGeometry(id, rect); }
void SystemController::onVideoFrameCaptured(const QImage& img, qint64 ts) { m_recorder->pushVideoFrame(img, ts); }
void SystemController::onSnapshotCaptured(const QImage& img) { QString f="Snap.png"; img.save(f); EventBus::instance()->fireSnapshotTaken(f); }
void SystemController::onDurationTimer() { qint64 s = m_recordElapsed.elapsed()/1000; EventBus::instance()->fireDurationUpdate(QString("%1:%2:%3").arg(s/3600,2,10,QChar('0')).arg((s%3600)/60,2,10,QChar('0')).arg(s%60,2,10,QChar('0'))); }

void SystemController::restartAudio(bool isMic, const QString& devId) {
    LOG_INFO(QString("Restarting Audio: %1").arg(isMic ? "Mic" : "Sys"));
    WasapiCapturer** ptr = isMic ? &m_micCapturer : &m_sysCapturer;
    if(*ptr) { (*ptr)->stop(); (*ptr)->wait(); delete *ptr; *ptr=nullptr; }
    *ptr = new WasapiCapturer(this); (*ptr)->setDevice(devId, isMic?WasapiCapturer::Microphone:WasapiCapturer::SystemLoopback);
    connect(*ptr, &WasapiCapturer::volumeLevelChanged, this, [isMic](float l){EventBus::instance()->fireAudioLevel(isMic, l);});
    connect(*ptr, &WasapiCapturer::audioDataReady, m_recorder, [this, isMic](QByteArray d, int r, int c){ if(m_recorder) m_recorder->pushAudioData(isMic?1:0, d, r, c); });
    (*ptr)->start();
}
