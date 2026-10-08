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
{
    setupUi();
    startCaptureThread();
}

MainWindow::~MainWindow() {
    stopCaptureThread();
    cleanupCameras();
}

void MainWindow::cleanupCameras() {
    for(auto ctx : m_cameras) {
        if (ctx->camera) ctx->camera->stop();
        if (ctx->workerThread) {
            ctx->workerThread->quit();
            ctx->workerThread->wait();
        }
        delete ctx->session;
        delete ctx->camera;
        delete ctx->sink;
        delete ctx->worker;
        delete ctx->workerThread;
        delete ctx;
    }
    m_cameras.clear();
}

void MainWindow::setupUi() {
    setWindowTitle("Qt OBS-Style Mixer (DXGI + WGC)");
    resize(1280, 720);
    QWidget *central = new QWidget(this); setCentralWidget(central);
    QVBoxLayout *layout = new QVBoxLayout(central);

    QHBoxLayout *topLayout = new QHBoxLayout();

    // 1. 采集源选择 (合并了显示器和窗口)
    topLayout->addWidget(new QLabel("Source:"));
    m_comboSources = new QComboBox();
    m_comboSources->setMinimumWidth(300);
    connect(m_comboSources, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onSourceChanged);
    topLayout->addWidget(m_comboSources);

    QPushButton* btnRefresh = new QPushButton("Refresh List");
    connect(btnRefresh, &QPushButton::clicked, this, &MainWindow::onRefreshSources);
    topLayout->addWidget(btnRefresh);

    // 2. 摄像头选择
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

    // 预览区
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

    // 底部控制
    QHBoxLayout *btnLayout = new QHBoxLayout();
    QPushButton *btnLoad = new QPushButton("Add Image Overlay");
    QPushButton *btnOut = new QPushButton("Snapshot");
    btnLayout->addWidget(btnLoad); btnLayout->addWidget(btnOut); layout->addLayout(btnLayout);
    connect(btnLoad, &QPushButton::clicked, this, &MainWindow::onLoadImage);
    connect(btnOut, &QPushButton::clicked, this, &MainWindow::onOutputResult);

    // 初始化列表
    onRefreshSources();
}

void MainWindow::onRefreshSources() {
    m_comboSources->blockSignals(true);
    m_comboSources->clear();

    // 添加显示器 (Monitor)
    QStringList monitors = DxGiCapturer::getMonitorNames();
    for (int i = 0; i < monitors.size(); ++i) {
        // UserData 存储: Type=0 (Monitor), ID=Index
        QList<QVariant> data; data << 0 << i;
        m_comboSources->addItem("[Monitor] " + monitors[i], data);
    }

    if (monitors.size() > 0) m_comboSources->insertSeparator(m_comboSources->count());

    // 添加窗口 (Window)
    QMap<size_t, QString> windows = DxGiCapturer::getWindowList();
    for (auto it = windows.begin(); it != windows.end(); ++it) {
        // UserData 存储: Type=1 (Window), ID=HWND
        QList<QVariant> data; data << 1 << (unsigned long long)it.key();
        m_comboSources->addItem("[Window] " + it.value(), data);
    }

    m_comboSources->blockSignals(false);

    // 如果有选项，触发一次切换
    if (m_comboSources->count() > 0) onSourceChanged(m_comboSources->currentIndex());
}

void MainWindow::onSourceChanged(int index) {
    if (index < 0 || !m_capturer) return;

    QVariant v = m_comboSources->itemData(index);
    if (!v.isValid()) return; // 分隔符

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
            // 线程启动后，根据当前 UI 选项设置目标
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

    // UI 预览信号
    connect(ctx->worker, &VideoSourceWorker::frameReadyForUI, this, [this, itemId](QImage img){
        onCameraFrameForUI(itemId, img);
    });

    // GPU 合成信号
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
