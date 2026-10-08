#include "SystemController.h"
#include "EventBus.h"
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
}

SystemController::~SystemController() {
    if (m_recorder) { m_recorder->stopRecord(); m_recorder->wait(); delete m_recorder; m_recorder = nullptr; }

    stopVideoThread();
    cleanupCameraPool();

    if (m_micCapturer) { m_micCapturer->stop(); m_micCapturer->wait(); delete m_micCapturer; }
    if (m_sysCapturer) { m_sysCapturer->stop(); m_sysCapturer->wait(); delete m_sysCapturer; }
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

    startVideoThread();
    QThread::msleep(200);
    onRefreshDevices();
    restartAudio(false, "");
}

void SystemController::startVideoThread() {
    if (m_videoThread) return;
    m_videoThread = new QThread(this);
    m_capturer = new DxGiCapturer();
    m_capturer->moveToThread(m_videoThread);

    connect(m_videoThread, &QThread::started, m_capturer, [this]() {
        // 初始化 COM/WinRT 环境 (在 Video 线程中持久存在)
        HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
        if (FAILED(hr)) CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        if (m_capturer->init()) {
            m_capturer->startCapture();
        } else {
            EventBus::instance()->fireError("Failed to initialize DXGI Capturer");
        }
    });

    // 监听 D3D 初始化完成，触发预热
    connect(m_capturer, &DxGiCapturer::initFinished, this, &SystemController::onCapturerInitFinished, Qt::QueuedConnection);

    connect(m_capturer, &DxGiCapturer::frameCaptured, this, &SystemController::onVideoFrameCaptured, Qt::QueuedConnection);
    connect(m_capturer, &DxGiCapturer::frameCapturedForPreview, EventBus::instance(), &EventBus::data_previewFrame, Qt::QueuedConnection);
    connect(m_capturer, &DxGiCapturer::snapshotCaptured, this, &SystemController::onSnapshotCaptured, Qt::QueuedConnection);
    connect(m_videoThread, &QThread::finished, m_capturer, &QObject::deleteLater);

    m_videoThread->start();
}

void SystemController::stopVideoThread() {
    if (m_capturer) QMetaObject::invokeMethod(m_capturer, "stopCapture", Qt::BlockingQueuedConnection);
    if (m_videoThread) { m_videoThread->quit(); m_videoThread->wait(3000); delete m_videoThread; m_videoThread = nullptr; m_capturer = nullptr; }
}

void SystemController::onCapturerInitFinished(bool success) {
    if (success) {
        qDebug() << "[System] GPU Init Done. Starting Camera Pre-warming...";
        prewarmAllCameras();
    }
}

void SystemController::prewarmAllCameras() {
    if (!m_capturer) return;

    // 【核心修复】不要使用 QtConcurrent::run，而是调度到 Video 线程执行
    // 这样 Camera 对象就会在 Video 线程的 COM 上下文中创建，且该线程一直存活
    QMetaObject::invokeMethod(m_capturer, [this]() {
        const auto cameras = QMediaDevices::videoInputs();
        // 获取 D3D 设备 (必须在 m_capturer 初始化后)
        ID3D11Device* rawDevice = m_capturer->getD3DDevice();
        if (!rawDevice) return;

        QMutexLocker locker(&m_poolMutex);

        for (const auto& info : cameras) {
            QString deviceId = QString::fromUtf8(info.id());
            QString name = info.description();
            if (m_cameraPool.contains(deviceId)) continue;

            qDebug() << "[Pre-warm] Initializing on VideoThread:" << name;
            WinRTCamera* cam = new WinRTCamera();

            // 此时我们在 Video 线程，可以直接调用 init/start
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
    // 放入 Video 线程清理
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

    for (auto ctx : m_activeLayers) {
        if (ctx->deviceId == deviceId) return;
    }

    // 尝试从池中获取
    WinRTCamera* targetCam = nullptr;
    {
        QMutexLocker locker(&m_poolMutex);
        if (m_cameraPool.contains(deviceId)) {
            targetCam = m_cameraPool.value(deviceId);
        }
    }

    // 定义添加图层的动作
    auto addLayerAction = [this, deviceId](WinRTCamera* cam) {
        if (!cam) return;
        int id = m_nextItemId++;
        CameraContext* ctx = new CameraContext;
        ctx->itemId = id;
        ctx->deviceId = deviceId;
        ctx->camera = cam;
        m_activeLayers.append(ctx);

        // 此时我们就在 Video 线程 (如果走 invokeMethod) 或 UI 线程
        // 为安全起见，再次 invoke 确保在 Video 线程操作 D3D
        QMetaObject::invokeMethod(m_capturer, [=](){
                m_capturer->addCameraOverlay(id, cam, QRect(50, 50, 320, 240));
            }, Qt::QueuedConnection);
        qDebug() << "[System] Camera Layer Added. ID:" << id;
    };

    if (targetCam) {
        qDebug() << "[System] Hit Pre-warmed Camera (Instant).";
        addLayerAction(targetCam);
    } else if (m_capturer) {
        qDebug() << "[System] Camera not warm, initializing on Video Thread...";

        // 【核心修复】调度到 Video 线程初始化
        QMetaObject::invokeMethod(m_capturer, [=]() {
            ID3D11Device* device = m_capturer->getD3DDevice();
            WinRTCamera* cam = new WinRTCamera();
            if (cam->init(device) && cam->start(deviceId)) {
                // 初始化成功，加入池
                { QMutexLocker l(&m_poolMutex); m_cameraPool.insert(deviceId, cam); }

                // 通知 UI 更新 (或者直接执行添加)
                // 注意：addLayerAction 内部涉及到 m_activeLayers (UI线程读写风险)，
                // 这里简单起见，我们直接调用 capturer 的添加，只在 UI 线程更新记录

                // 1. 直接在底层添加覆盖层
                int id = m_nextItemId++; // 注意：原子操作或加锁
                m_capturer->addCameraOverlay(id, cam, QRect(50, 50, 320, 240));

                // 2. 回调 UI 线程更新状态
                QMetaObject::invokeMethod(this, [=]() {
                    CameraContext* ctx = new CameraContext;
                    ctx->itemId = id;
                    ctx->deviceId = deviceId;
                    ctx->camera = cam;
                    m_activeLayers.append(ctx);
                    qDebug() << "[System] Camera Layer Added (Lazy Init). ID:" << id;
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
    WasapiCapturer** ptr = isMic ? &m_micCapturer : &m_sysCapturer;
    if(*ptr) { (*ptr)->stop(); (*ptr)->wait(); delete *ptr; *ptr=nullptr; }
    *ptr = new WasapiCapturer(this); (*ptr)->setDevice(devId, isMic?WasapiCapturer::Microphone:WasapiCapturer::SystemLoopback);
    connect(*ptr, &WasapiCapturer::volumeLevelChanged, this, [isMic](float l){EventBus::instance()->fireAudioLevel(isMic, l);});
    connect(*ptr, &WasapiCapturer::audioDataReady, m_recorder, [this, isMic](QByteArray d, int r, int c){ if(m_recorder) m_recorder->pushAudioData(isMic?1:0, d, r, c); });
    (*ptr)->start();
}
