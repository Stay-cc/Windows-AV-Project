#include "MainWindow.h"
#include "ResizablePixmapItem.h"
#include "DxGiCapturer.h"
#include "VideoSourceWorker.h"
#include "WasapiCapturer.h" // 包含头文件
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QFileDialog>
#include <QDateTime>
#include <QDebug>
#include <QMessageBox>
#include <QLabel>
#include <QGroupBox>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), m_captureThread(nullptr), m_capturer(nullptr)
{
    setupUi();
    startCaptureThread();
    startAudioCapture(); // 启动默认音频采集
}

MainWindow::~MainWindow() {
    stopAudioCapture();
    stopCaptureThread();
    cleanupCameras();
}

void MainWindow::setupUi() {
    setWindowTitle("Qt OBS-Style Mixer (DXGI + WGC + WASAPI Audio)");
    resize(1280, 800);
    QWidget *central = new QWidget(this); setCentralWidget(central);
    QVBoxLayout *layout = new QVBoxLayout(central);

    // --- 顶部控制区域 ---
    QHBoxLayout *topLayout = new QHBoxLayout();

    // 1. 视频源
    QGroupBox *grpVideo = new QGroupBox("Video Sources");
    QVBoxLayout *vboxVideo = new QVBoxLayout(grpVideo);

    QHBoxLayout *hboxSrc = new QHBoxLayout();
    hboxSrc->addWidget(new QLabel("Display/Window:"));
    m_comboSources = new QComboBox();
    m_comboSources->setMinimumWidth(200);
    connect(m_comboSources, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onSourceChanged);
    hboxSrc->addWidget(m_comboSources);
    QPushButton* btnRefresh = new QPushButton("Refresh");
    connect(btnRefresh, &QPushButton::clicked, this, &MainWindow::onRefreshSources);
    hboxSrc->addWidget(btnRefresh);
    vboxVideo->addLayout(hboxSrc);

    QHBoxLayout *hboxCam = new QHBoxLayout();
    hboxCam->addWidget(new QLabel("Webcam:"));
    m_comboCameras = new QComboBox();
    const QList<QCameraDevice> cameras = QMediaDevices::videoInputs();
    for (const QCameraDevice &cameraDevice : cameras) {
        m_comboCameras->addItem(cameraDevice.description(), QVariant::fromValue(cameraDevice));
    }
    hboxCam->addWidget(m_comboCameras);
    QPushButton *btnAddCam = new QPushButton("Add Cam");
    connect(btnAddCam, &QPushButton::clicked, this, &MainWindow::onAddCamera);
    hboxCam->addWidget(btnAddCam);
    vboxVideo->addLayout(hboxCam);

    topLayout->addWidget(grpVideo);

    // 2. 音频源 (新增)
    QGroupBox *grpAudio = new QGroupBox("Audio Mixer (WASAPI)");
    QVBoxLayout *vboxAudio = new QVBoxLayout(grpAudio);

    // 2.1 系统声音 (Desktop Audio)
    QHBoxLayout *hboxSys = new QHBoxLayout();
    hboxSys->addWidget(new QLabel("Desktop:"));
    m_comboSysAudio = new QComboBox();
    m_comboSysAudio->setMinimumWidth(150);
    hboxSys->addWidget(m_comboSysAudio);

    m_progressSys = new QProgressBar();
    m_progressSys->setRange(0, 100);
    m_progressSys->setValue(0);
    m_progressSys->setTextVisible(false);
    m_progressSys->setFixedWidth(100);
    m_progressSys->setStyleSheet("QProgressBar::chunk { background-color: #00ff00; }");
    hboxSys->addWidget(m_progressSys);
    vboxAudio->addLayout(hboxSys);

    // 2.2 麦克风 (Mic/Aux)
    QHBoxLayout *hboxMic = new QHBoxLayout();
    hboxMic->addWidget(new QLabel("Mic/Aux:"));
    m_comboMic = new QComboBox();
    m_comboMic->setMinimumWidth(150);
    hboxMic->addWidget(m_comboMic);

    m_progressMic = new QProgressBar();
    m_progressMic->setRange(0, 100);
    m_progressMic->setValue(0);
    m_progressMic->setTextVisible(false);
    m_progressMic->setFixedWidth(100);
    m_progressMic->setStyleSheet("QProgressBar::chunk { background-color: #00ffff; }");
    hboxMic->addWidget(m_progressMic);
    vboxAudio->addLayout(hboxMic);

    QPushButton *btnRefreshAudio = new QPushButton("Refresh Devices");
    connect(btnRefreshAudio, &QPushButton::clicked, this, &MainWindow::onRefreshAudioDevices);
    vboxAudio->addWidget(btnRefreshAudio);

    topLayout->addWidget(grpAudio);

    layout->addLayout(topLayout);

    // --- 中间预览区 ---
    m_scene = new QGraphicsScene(this);
    m_scene->setSceneRect(0, 0, 1920, 1080);
    m_view = new QGraphicsView(m_scene);
    m_view->setRenderHint(QPainter::Antialiasing);
    m_view->setViewportUpdateMode(QGraphicsView::BoundingRectViewportUpdate);
    m_view->setBackgroundBrush(Qt::black);
    layout->addWidget(m_view);

    m_bgItem = new QGraphicsPixmapItem();
    m_bgItem->setZValue(0);
    m_scene->addItem(m_bgItem);

    // --- 底部控制 ---
    QHBoxLayout *btnLayout = new QHBoxLayout();
    QPushButton *btnLoad = new QPushButton("Add Image Overlay");
    QPushButton *btnOut = new QPushButton("Snapshot");
    btnLayout->addWidget(btnLoad); btnLayout->addWidget(btnOut); layout->addLayout(btnLayout);
    connect(btnLoad, &QPushButton::clicked, this, &MainWindow::onLoadImage);
    connect(btnOut, &QPushButton::clicked, this, &MainWindow::onOutputResult);

    // 信号连接
    connect(m_comboMic, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onMicChanged);
    connect(m_comboSysAudio, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onSystemAudioChanged);

    // 初始化列表
    onRefreshSources();
    onRefreshAudioDevices();
}

// 刷新音频设备列表
void MainWindow::onRefreshAudioDevices() {
    m_comboMic->blockSignals(true);
    m_comboSysAudio->blockSignals(true);
    m_comboMic->clear();
    m_comboSysAudio->clear();

    // 1. 获取麦克风列表
    QList<AudioDeviceInfo> mics = WasapiCapturer::getAvailableDevices(WasapiCapturer::Microphone);
    m_comboMic->addItem("Default Microphone", ""); // 空字符串代表默认
    for(const auto& dev : mics) {
        m_comboMic->addItem(dev.name, dev.id);
    }

    // 2. 获取系统播放设备列表 (用于 Loopback)
    QList<AudioDeviceInfo> speakers = WasapiCapturer::getAvailableDevices(WasapiCapturer::SystemLoopback);
    m_comboSysAudio->addItem("Default Desktop Audio", "");
    for(const auto& dev : speakers) {
        m_comboSysAudio->addItem(dev.name, dev.id);
    }

    m_comboMic->blockSignals(false);
    m_comboSysAudio->blockSignals(false);
}

void MainWindow::startAudioCapture() {
    // 麦克风采集线程
    m_micCapturer = new WasapiCapturer(this);
    connect(m_micCapturer, &WasapiCapturer::volumeLevelChanged, this, [this](float level){
        onAudioVolumeChanged(true, level);
    });
    // 可以在这里 connect audioDataReady 信号来获取原始数据写入文件
    m_micCapturer->setDevice("", WasapiCapturer::Microphone); // 默认
    m_micCapturer->start();

    // 系统声音采集线程 (Loopback)
    m_sysCapturer = new WasapiCapturer(this);
    connect(m_sysCapturer, &WasapiCapturer::volumeLevelChanged, this, [this](float level){
        onAudioVolumeChanged(false, level);
    });
    m_sysCapturer->setDevice("", WasapiCapturer::SystemLoopback); // 默认
    m_sysCapturer->start();
}

void MainWindow::stopAudioCapture() {
    if (m_micCapturer) {
        m_micCapturer->stop();
        m_micCapturer->wait();
        delete m_micCapturer;
        m_micCapturer = nullptr;
    }
    if (m_sysCapturer) {
        m_sysCapturer->stop();
        m_sysCapturer->wait();
        delete m_sysCapturer;
        m_sysCapturer = nullptr;
    }
}

void MainWindow::onMicChanged(int index) {
    if (!m_micCapturer) return;
    QString id = m_comboMic->itemData(index).toString();
    // 需要重启线程来切换设备
    m_micCapturer->stop();
    m_micCapturer->wait();
    m_micCapturer->setDevice(id, WasapiCapturer::Microphone);
    m_micCapturer->start();
}

void MainWindow::onSystemAudioChanged(int index) {
    if (!m_sysCapturer) return;
    QString id = m_comboSysAudio->itemData(index).toString();
    m_sysCapturer->stop();
    m_sysCapturer->wait();
    m_sysCapturer->setDevice(id, WasapiCapturer::SystemLoopback);
    m_sysCapturer->start();
}

void MainWindow::onAudioVolumeChanged(bool isMic, float level) {
    int val = static_cast<int>(level * 100);
    if (isMic) m_progressMic->setValue(val);
    else m_progressSys->setValue(val);
}

// ... 下面是原有的视频相关代码 ...
void MainWindow::onRefreshSources() {
    m_comboSources->blockSignals(true);
    m_comboSources->clear();

    QStringList monitors = DxGiCapturer::getMonitorNames();
    for (int i = 0; i < monitors.size(); ++i) {
        QList<QVariant> data; data << 0 << i;
        m_comboSources->addItem("[Monitor] " + monitors[i], data);
    }

    if (monitors.size() > 0) m_comboSources->insertSeparator(m_comboSources->count());

    QMap<size_t, QString> windows = DxGiCapturer::getWindowList();
    for (auto it = windows.begin(); it != windows.end(); ++it) {
        QList<QVariant> data; data << 1 << (unsigned long long)it.key();
        m_comboSources->addItem("[Window] " + it.value(), data);
    }

    m_comboSources->blockSignals(false);
    if (m_comboSources->count() > 0) onSourceChanged(m_comboSources->currentIndex());
}

void MainWindow::onSourceChanged(int index) {
    if (index < 0 || !m_capturer) return;
    QVariant v = m_comboSources->itemData(index);
    if (!v.isValid()) return;
    QList<QVariant> list = v.toList();
    int type = list[0].toInt();
    CaptureTarget target;
    if (type == 0) {
        target.type = CaptureSourceType::Monitor;
        target.monitorIndex = list[1].toInt();
    } else {
        target.type = CaptureSourceType::Window;
        target.windowHandle = (HWND)list[1].toULongLong();
    }
    m_capturer->setCaptureTarget(target);
}

void MainWindow::startCaptureThread() {
    if (m_captureThread) return;
    m_captureThread = new QThread(this);
    m_capturer = new DxGiCapturer();
    m_capturer->moveToThread(m_captureThread);
    connect(m_captureThread, &QThread::started, m_capturer, [this]() {
        if (m_capturer->init()) {
            QMetaObject::invokeMethod(this, [this](){
                onSourceChanged(m_comboSources->currentIndex());
            });
            m_capturer->startCapture();
        }
    });
    connect(m_capturer, &DxGiCapturer::frameCaptured, this, &MainWindow::onFrameCaptured);
    connect(m_capturer, &DxGiCapturer::snapshotCaptured, this, &MainWindow::onSnapshotCaptured);
    connect(m_captureThread, &QThread::finished, m_capturer, &QObject::deleteLater);
    m_captureThread->start();
}

void MainWindow::stopCaptureThread() {
    if (m_capturer) m_capturer->stopCapture();
    if (m_captureThread) {
        m_captureThread->quit();
        m_captureThread->wait();
        delete m_captureThread;
        m_captureThread = nullptr;
        m_capturer = nullptr;
    }
}

void MainWindow::onFrameCaptured(QImage img) {
    if (m_scene->sceneRect().size() != img.size()) { m_scene->setSceneRect(img.rect()); m_bgItem->setPos(0, 0); }
    m_bgItem->setPixmap(QPixmap::fromImage(img));
}

void MainWindow::onLoadImage() {
    QString path = QFileDialog::getOpenFileName(this, "Select Image", "", "Images (*.png *.jpg *.bmp)");
    if (!path.isEmpty()) {
        QPixmap pix(path);
        if (!pix.isNull()) {
            int id = m_nextItemId++;
            ResizablePixmapItem *item = new ResizablePixmapItem(pix, id);
            item->setZValue(10);
            QRectF r = m_scene->sceneRect();
            item->setPos((r.width() - pix.width())/2, (r.height() - pix.height())/2);
            m_scene->addItem(item);
            item->setSelected(true);
            connect(item, &ResizablePixmapItem::geometryChanged, this, &MainWindow::onItemGeometryChanged);
            if (m_capturer) {
                QRect finalRect(item->x(), item->y(), item->getImageRect().width(), item->getImageRect().height());
                m_capturer->addOverlay(id, pix.toImage(), finalRect);
            }
        }
    }
}

void MainWindow::onCameraFrameForUI(int itemId, QImage img) {
    if (m_cameraItems.contains(itemId)) {
        m_cameraItems[itemId]->setPixmap(QPixmap::fromImage(img));
    }
}

void MainWindow::onAddCamera() {
    int index = m_comboCameras->currentIndex();
    if (index == -1) return;
    QCameraDevice device = m_comboCameras->itemData(index).value<QCameraDevice>();
    int itemId = m_nextItemId++;
    QPixmap placeholder(320, 240); placeholder.fill(Qt::darkGray);
    ResizablePixmapItem *item = new ResizablePixmapItem(placeholder, itemId);
    item->setZValue(20); item->setPos(50, 50);
    m_scene->addItem(item); item->setSelected(true);
    m_cameraItems.insert(itemId, item);
    connect(item, &ResizablePixmapItem::geometryChanged, this, &MainWindow::onItemGeometryChanged);
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
    connect(ctx->worker, &VideoSourceWorker::frameReadyForUI, this, [this, itemId](QImage img){ onCameraFrameForUI(itemId, img); });
    if (m_capturer) {
        connect(ctx->worker, &VideoSourceWorker::frameReadyForCompositor, m_capturer, &DxGiCapturer::updateOverlayImage, Qt::QueuedConnection);
        m_capturer->addOverlay(itemId, placeholder.toImage(), QRect(50, 50, 320, 240));
    }
    ctx->workerThread->start();
    ctx->camera->start();
    m_cameras.append(ctx);
}

void MainWindow::onItemGeometryChanged(int id, QRect rect) {
    if (m_capturer) m_capturer->updateOverlayGeometry(id, rect);
}

void MainWindow::onOutputResult() {
    if (m_capturer) m_capturer->requestSnapshot();
}

void MainWindow::onSnapshotCaptured(QImage img) {
    QString fileName = "Output_" + QDateTime::currentDateTime().toString("HHmmss") + ".png";
    img.save(fileName);
    QMessageBox::information(this, "Saved", "Saved successfully!\n" + fileName);
}

void MainWindow::cleanupCameras() {
    for(auto ctx : m_cameras) {
        if (ctx->camera) ctx->camera->stop();
        if (ctx->workerThread) { ctx->workerThread->quit(); ctx->workerThread->wait(); }
        delete ctx->session; delete ctx->camera; delete ctx->sink; delete ctx->worker; delete ctx->workerThread; delete ctx;
    }
    m_cameras.clear();
}
