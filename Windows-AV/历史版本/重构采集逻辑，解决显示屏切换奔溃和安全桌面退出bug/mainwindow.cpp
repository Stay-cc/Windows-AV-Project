#include "MainWindow.h"
#include "ResizablePixmapItem.h"
#include "DxGiCapturer.h"
#include "VideoSourceWorker.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QFileDialog>
#include <QDateTime>
#include <QDebug>
#include <QMessageBox>
#include <QLabel>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), m_captureThread(nullptr), m_capturer(nullptr)
    , m_pendingRestart(false), m_pendingMonitorIndex(0)
{
    setupUi();
    startCaptureThread(0);
}

MainWindow::~MainWindow() {
    stopCaptureThread();
    cleanupCameras();
}

void MainWindow::cleanupCameras() {
    for(auto ctx : m_cameras) {
        if (ctx->camera) ctx->camera->stop();

        // 停止 Worker 线程
        if (ctx->workerThread) {
            ctx->workerThread->quit();
            ctx->workerThread->wait();
        }

        // 资源会在线程结束时由 deleteLater 自动处理，或者手动清理
        delete ctx->session;
        delete ctx->camera;
        delete ctx->sink;
        delete ctx->worker; // worker 是普通 QObject
        delete ctx->workerThread;
        delete ctx;
    }
    m_cameras.clear();
}

void MainWindow::setupUi() {
    setWindowTitle("Qt DXGI GPU Mixer - Enterprise Grade");
    resize(1280, 720);
    QWidget *central = new QWidget(this); setCentralWidget(central);
    QVBoxLayout *layout = new QVBoxLayout(central);

    QHBoxLayout *topLayout = new QHBoxLayout();
    topLayout->addWidget(new QLabel("Monitor:"));
    m_comboMonitors = new QComboBox();
    QStringList monitors = DxGiCapturer::getMonitorNames();
    if (monitors.isEmpty()) m_comboMonitors->addItem("Default (0)");
    else m_comboMonitors->addItems(monitors);
    connect(m_comboMonitors, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onMonitorChanged);
    topLayout->addWidget(m_comboMonitors);

    topLayout->addWidget(new QLabel("  Webcam:"));
    m_comboCameras = new QComboBox();
    const QList<QCameraDevice> cameras = QMediaDevices::videoInputs();
    for (const QCameraDevice &cameraDevice : cameras) {
        m_comboCameras->addItem(cameraDevice.description(), QVariant::fromValue(cameraDevice));
    }
    topLayout->addWidget(m_comboCameras);

    QPushButton *btnAddCam = new QPushButton("Add Camera");
    connect(btnAddCam, &QPushButton::clicked, this, &MainWindow::onAddCamera);
    topLayout->addWidget(btnAddCam);

    topLayout->addStretch();
    layout->addLayout(topLayout);

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

    QHBoxLayout *btnLayout = new QHBoxLayout();
    QPushButton *btnLoad = new QPushButton("Add Image Overlay");
    QPushButton *btnOut = new QPushButton("Save Output (From GPU)");
    btnLayout->addWidget(btnLoad); btnLayout->addWidget(btnOut); layout->addLayout(btnLayout);
    connect(btnLoad, &QPushButton::clicked, this, &MainWindow::onLoadImage);
    connect(btnOut, &QPushButton::clicked, this, &MainWindow::onOutputResult);
}

void MainWindow::startCaptureThread(int monitorIndex) {
    if (m_captureThread) return;
    m_captureThread = new QThread(this);
    m_capturer = new DxGiCapturer();
    m_capturer->moveToThread(m_captureThread);

    connect(m_captureThread, &QThread::started, m_capturer, [this, monitorIndex]() {
        if (m_capturer->init(monitorIndex)) m_capturer->startCapture();
    });

    connect(m_capturer, &DxGiCapturer::frameCaptured, this, &MainWindow::onFrameCaptured); // UI 预览
    connect(m_capturer, &DxGiCapturer::snapshotCaptured, this, &MainWindow::onSnapshotCaptured);
    connect(m_capturer, &DxGiCapturer::captureStopped, this, [this](){
        if (m_pendingRestart) {
            m_captureThread->quit(); m_captureThread->wait(); delete m_captureThread; m_captureThread = nullptr; m_capturer = nullptr;
            startCaptureThread(m_pendingMonitorIndex); m_pendingRestart = false;
        }
    });

    connect(m_captureThread, &QThread::finished, m_capturer, &QObject::deleteLater);
    m_captureThread->start();
}

void MainWindow::stopCaptureThread() {
    if (m_capturer) m_capturer->stopCapture();
    if (m_captureThread) { m_captureThread->quit(); m_captureThread->wait(); delete m_captureThread; m_captureThread = nullptr; m_capturer = nullptr; }
}

void MainWindow::onMonitorChanged(int index) {
    if (!m_captureThread) { startCaptureThread(index); return; }
    m_pendingRestart = true; m_pendingMonitorIndex = index;
    if (m_capturer) m_capturer->stopCapture();
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
    // 检查该 ID 对应的图层是否存在
    if (m_cameraItems.contains(itemId)) {
        // 调用 ResizablePixmapItem 的 setPixmap 更新画面
        m_cameraItems[itemId]->setPixmap(QPixmap::fromImage(img));
    }
}

// ---------------------------------------------------------
// 摄像头核心逻辑 (企业级 Worker Thread 模式)
// ---------------------------------------------------------
void MainWindow::onAddCamera() {
    int index = m_comboCameras->currentIndex();
    if (index == -1) return;

    QCameraDevice device = m_comboCameras->itemData(index).value<QCameraDevice>();

    // 创建 UI 占位图
    int itemId = m_nextItemId++;
    QPixmap placeholder(320, 240); placeholder.fill(Qt::darkGray);
    ResizablePixmapItem *item = new ResizablePixmapItem(placeholder, itemId);
    item->setZValue(20); item->setPos(50, 50);
    m_scene->addItem(item); item->setSelected(true);
    m_cameraItems.insert(itemId, item);

    connect(item, &ResizablePixmapItem::geometryChanged, this, &MainWindow::onItemGeometryChanged);

    // 初始化相机
    CameraContext* ctx = new CameraContext;
    ctx->itemId = itemId;
    ctx->camera = new QCamera(device);
    ctx->session = new QMediaCaptureSession;
    ctx->sink = new QVideoSink;
    ctx->session->setCamera(ctx->camera);
    ctx->session->setVideoOutput(ctx->sink);

    // 【核心】创建独立 Worker 线程处理视频流
    ctx->workerThread = new QThread(this);
    ctx->worker = new VideoSourceWorker(ctx->sink, itemId);
    ctx->worker->moveToThread(ctx->workerThread);

    // 连接信号：VideoSink -> Worker
    connect(ctx->sink, &QVideoSink::videoFrameChanged, ctx->worker, &VideoSourceWorker::processFrame, Qt::QueuedConnection);

    // 连接信号：Worker -> UI (预览)
    connect(ctx->worker, &VideoSourceWorker::frameReadyForUI, this, [this, itemId](QImage img){
        if (m_cameraItems.contains(itemId)) {
            m_cameraItems[itemId]->setPixmap(QPixmap::fromImage(img));
        }
    });

    // 连接信号：Worker -> Compositor (DXGI混流)
    // 注意：这里需要跨线程传递数据到 DxGiCapturer
    // 由于 DxGiCapturer 在 m_captureThread，所以信号会自动排队
    if (m_capturer) {
        connect(ctx->worker, &VideoSourceWorker::frameReadyForCompositor, m_capturer, &DxGiCapturer::updateOverlayImage, Qt::QueuedConnection);

        // 初始添加到合成器
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
    QString fileName = "DX_Output_" + QDateTime::currentDateTime().toString("HHmmss") + ".png";
    img.save(fileName);
    QMessageBox::information(this, "Saved", "Saved successfully!\n" + fileName);
}
