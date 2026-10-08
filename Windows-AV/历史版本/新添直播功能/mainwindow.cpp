#include "MainWindow.h"
#include "SettingsDialog.h"
#include "ResizablePixmapItem.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QMessageBox>
#include <QFileDialog>
#include <QTimer>
#include <QMediaDevices>
#include <QCameraDevice>
#include "Logger.h"
#include <QPainter>

MainWindow::MainWindow(QWidget *parent) : FramelessWidget(parent)
{
    setupUi();

    setFixedSize(1060, 630);
    setTitleBarHeight(40);
    applyWindowConfig();
    m_lastSceneSize = QSize(0, 0); // 初始化

    EventBus* bus = EventBus::instance();
    connect(bus, &EventBus::data_previewFrame, this, &MainWindow::updatePreview);
    connect(bus, &EventBus::data_overlayUpdate, this, &MainWindow::updateOverlay);
    connect(bus, &EventBus::data_audioLevel, this, &MainWindow::updateAudioLevel);
    connect(bus, &EventBus::state_recordingChanged, this, &MainWindow::onRecordingStateChanged);
    connect(bus, &EventBus::state_durationUpdated, this, &MainWindow::onDurationUpdate);
    connect(bus, &EventBus::state_sourceListUpdated, this, &MainWindow::onSourceListUpdated);
    connect(bus, &EventBus::state_audioDeviceListUpdated, this, &MainWindow::onAudioDeviceListUpdated);
    connect(bus, &EventBus::state_snapshotTaken, this, &MainWindow::onSnapshotTaken);
    connect(bus, &EventBus::state_error, this, &MainWindow::onError);

    // 【核心】连接添加摄像头的信号
    connect(bus, &EventBus::state_cameraAdded, this, &MainWindow::onCameraAdded);

    QTimer::singleShot(500, this, [this](){
        LOG_INFO("UI Safe-Init: Refreshing Cameras & Devices...");
        this->refreshCameras();
        EventBus::instance()->sendCommandRefreshDevices();
    });
}

MainWindow::~MainWindow() {}

void MainWindow::refreshCameras() {
    m_comboCameras->clear();
    const QList<QCameraDevice> cameras = QMediaDevices::videoInputs();
    for (const QCameraDevice &c : cameras) {
        m_comboCameras->addItem(c.description(), QVariant::fromValue(c));
    }
    LOG_INFO(QString("MainWindow: Found %1 cameras.").arg(cameras.size()));
}

void MainWindow::applyWindowConfig() {
    setResizable(m_config.windowResizable);
    setMaximizable(m_config.windowMaximizable);
    if (m_btnMax) m_btnMax->setEnabled(m_config.windowMaximizable);
}

void MainWindow::setupUi() {
    setStyleSheet(R"(
        QWidget { background-color: #2b2b2b; color: #ffffff; font-family: 'Segoe UI'; }
        QGroupBox { border: 1px solid #444; margin-top: 10px; border-radius: 4px; }
        QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 3px; }
        QLabel { color: #cccccc; }
        QPushButton { background-color: #3e3e3e; border: 1px solid #555; border-radius: 4px; padding: 5px 10px; }
        QPushButton:hover { background-color: #505050; border-color: #666; }
        QPushButton:pressed { background-color: #2d2d2d; }
        QComboBox { background-color: #333; border: 1px solid #555; padding: 4px; border-radius: 3px; }
        QProgressBar { border: 1px solid #555; border-radius: 3px; text-align: center; background: #1a1a1a; }
        QPushButton#TitleBtn { background: transparent; border: none; }
        QPushButton#TitleBtn:hover { background: #444; }
        QPushButton#CloseBtn { background: transparent; border: none; }
        QPushButton#CloseBtn:hover { background: #e81123; }
    )");

    QVBoxLayout* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    setupTitleBar(rootLayout);

    QWidget* contentWidget = new QWidget();
    rootLayout->addWidget(contentWidget);

    QVBoxLayout* layout = new QVBoxLayout(contentWidget);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(10);

    // Video Sources
    QHBoxLayout *topLayout = new QHBoxLayout();
    QGroupBox *grpVideo = new QGroupBox("Video Sources");
    QVBoxLayout *vboxVideo = new QVBoxLayout(grpVideo);

    QHBoxLayout *hboxSrc = new QHBoxLayout();
    hboxSrc->addWidget(new QLabel("Source:"));
    m_comboSources = new QComboBox(); m_comboSources->setMinimumWidth(180);
    connect(m_comboSources, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onSourceComboChanged);
    hboxSrc->addWidget(m_comboSources);
    QPushButton* btnRefresh = new QPushButton("⟳"); btnRefresh->setFixedWidth(30);
    connect(btnRefresh, &QPushButton::clicked, this, &MainWindow::onBtnRefreshClicked);
    hboxSrc->addWidget(btnRefresh);
    vboxVideo->addLayout(hboxSrc);

    QHBoxLayout *hboxCam = new QHBoxLayout();
    hboxCam->addWidget(new QLabel("Webcam:"));
    m_comboCameras = new QComboBox();
    m_comboCameras->addItem("Initializing...");
    hboxCam->addWidget(m_comboCameras);
    QPushButton *btnAddCam = new QPushButton("+"); btnAddCam->setFixedWidth(30);
    connect(btnAddCam, &QPushButton::clicked, this, &MainWindow::onBtnAddCamClicked);
    hboxCam->addWidget(btnAddCam);
    vboxVideo->addLayout(hboxCam);
    topLayout->addWidget(grpVideo);

    // Audio Group
    QGroupBox *grpAudio = new QGroupBox("Audio Mixer");
    QVBoxLayout *vboxAudio = new QVBoxLayout(grpAudio);

    QHBoxLayout *hboxSys = new QHBoxLayout();
    hboxSys->addWidget(new QLabel("System:"));
    m_comboSysAudio = new QComboBox(); m_comboSysAudio->setMinimumWidth(150);
    connect(m_comboSysAudio, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onSysComboChanged);
    hboxSys->addWidget(m_comboSysAudio);
    m_progressSys = new QProgressBar(); m_progressSys->setRange(0, 100);
    m_progressSys->setStyleSheet("QProgressBar::chunk { background-color: #4CAF50; }");
    hboxSys->addWidget(m_progressSys);
    vboxAudio->addLayout(hboxSys);

    m_chkMixMic = new QCheckBox("Mix Microphone");
    connect(m_chkMixMic, &QCheckBox::toggled, this, &MainWindow::onChkMixMicToggled);
    vboxAudio->addWidget(m_chkMixMic);

    QHBoxLayout *hboxMic = new QHBoxLayout();
    hboxMic->addWidget(new QLabel("Mic:     "));
    m_comboMic = new QComboBox(); m_comboMic->setMinimumWidth(150); m_comboMic->setEnabled(false);
    connect(m_comboMic, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onMicComboChanged);
    hboxMic->addWidget(m_comboMic);
    m_progressMic = new QProgressBar(); m_progressMic->setRange(0, 100); m_progressMic->setEnabled(false);
    m_progressMic->setStyleSheet("QProgressBar::chunk { background-color: #2196F3; }");
    hboxMic->addWidget(m_progressMic);
    vboxAudio->addLayout(hboxMic);
    topLayout->addWidget(grpAudio);

    layout->addLayout(topLayout);

    // Preview
    m_scene = new QGraphicsScene(this);
    m_view = new QGraphicsView(m_scene);
    m_view->setRenderHint(QPainter::Antialiasing);
    m_view->setRenderHint(QPainter::SmoothPixmapTransform);
    m_view->setBackgroundBrush(Qt::black);
    m_view->setFrameShape(QFrame::NoFrame);
    m_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(m_view);

    QPixmap defaultBg(1920, 1080);
    defaultBg.fill(Qt::black);
    m_bgItem = new QGraphicsPixmapItem(defaultBg);
    m_scene->addItem(m_bgItem);

    // Controls
    QHBoxLayout *btnLayout = new QHBoxLayout();
    m_btnSettings = new QPushButton("⚙ Settings");
    connect(m_btnSettings, &QPushButton::clicked, this, &MainWindow::onBtnSettingsClicked);
    btnLayout->addWidget(m_btnSettings);

    m_lblDuration = new QLabel("00:00:00");
    m_lblDuration->setStyleSheet("color: #ff5555; font-weight: bold; font-size: 16px; margin-right: 15px;");
    m_lblDuration->setVisible(false);
    btnLayout->addWidget(m_lblDuration);

    QPushButton *btnLoad = new QPushButton("Add Image");
    connect(btnLoad, &QPushButton::clicked, this, &MainWindow::onBtnLoadImageClicked);
    btnLayout->addWidget(btnLoad);

    QPushButton *btnSnap = new QPushButton("Snapshot");
    connect(btnSnap, &QPushButton::clicked, this, &MainWindow::onBtnSnapshotClicked);
    btnLayout->addWidget(btnSnap);

    m_btnRecord = new QPushButton("● Start Recording");
    m_btnRecord->setMinimumHeight(36);
    m_btnRecord->setStyleSheet("QPushButton { background-color: #2a2a2a; border: 1px solid #444; font-weight: bold; } QPushButton:hover{ background: #333; }");
    connect(m_btnRecord, &QPushButton::clicked, this, &MainWindow::onBtnRecordClicked);
    btnLayout->addWidget(m_btnRecord);

    layout->addLayout(btnLayout);
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_view && m_scene) {
        QRectF rect = m_scene->sceneRect();
        if (rect.width() > 1 && rect.height() > 1) {
            m_view->fitInView(rect, Qt::KeepAspectRatio);
        }
    }
}

// 【新增】处理摄像头添加，生成透明控制框
void MainWindow::onCameraAdded(int id, QRect rect) {
    // 创建一个透明的 Pixmap，但画上边框让用户能看到
    QPixmap placeholder(rect.size());
    placeholder.fill(QColor(0, 0, 0, 0)); // 全透明背景

    QPainter p(&placeholder);
    p.setRenderHint(QPainter::Antialiasing);
    // 画一个半透明白色边框，表示“这里有摄像头”
    p.setPen(QPen(QColor(255, 255, 255, 150), 4, Qt::DashLine));
    p.drawRect(0, 0, rect.width(), rect.height());
    // 画文字
    p.setPen(Qt::white);
    QFont font = p.font();
    font.setBold(true);
    font.setPointSize(14);
    p.setFont(font);
    p.drawText(placeholder.rect(), Qt::AlignCenter, "");

    // 创建可交互对象
    ResizablePixmapItem *item = new ResizablePixmapItem(placeholder, id);
    item->setPos(rect.topLeft());
    item->setZValue(100); // 确保在最上层，可以点击到
    m_scene->addItem(item);

    // 默认选中，让用户知道添加成功了
    item->setSelected(true);

    // 存入 Map 管理
    m_overlayItems.insert(id, item);

    // 连接几何变化信号 -> 通知后台调整渲染位置
    connect(item, &ResizablePixmapItem::geometryChanged, this, &MainWindow::onItemGeometryChanged);
}

void MainWindow::updatePreview(QImage img) {
    if (img.isNull() || img.width() <= 1 || img.height() <= 1) return;

    // 1. 检测分辨率是否发生变化
    if (m_lastSceneSize.isValid() && !m_lastSceneSize.isEmpty() && m_lastSceneSize != img.size()) {
        // 计算宽高的缩放比例
        qreal scaleX = (qreal)img.width() / m_lastSceneSize.width();
        qreal scaleY = (qreal)img.height() / m_lastSceneSize.height();

        LOG_INFO(QString("Resolution Changed: %1x%2 -> %3x%4 (Scale: %5, %6)")
                     .arg(m_lastSceneSize.width()).arg(m_lastSceneSize.height())
                     .arg(img.width()).arg(img.height())
                     .arg(scaleX).arg(scaleY));

        // 2. 遍历所有图层（摄像头/图片），按比例调整位置和大小
        for (auto item : m_overlayItems) {
            // 获取当前的绝对位置和大小
            QPointF oldPos = item->pos();
            QRectF oldRect = item->getImageRect();

            // 计算新的相对位置
            qreal newX = oldPos.x() * scaleX;
            qreal newY = oldPos.y() * scaleY;
            qreal newW = oldRect.width() * scaleX;
            qreal newH = oldRect.height() * scaleY;

            // 应用新的几何变换
            item->setPos(newX, newY);
            item->setRect(QRectF(0, 0, newW, newH));

            // 【关键】立即通知后台 (DxGiCapturer) 更新渲染坐标
            // 这样后台渲染的摄像头位置就能立刻对齐到新分辨率
            EventBus::instance()->sendOverlayGeometryChanged(
                item->getId(),
                QRect(newX, newY, newW, newH)
                );
        }
    }

    // 更新记录的分辨率
    m_lastSceneSize = img.size();

    // ... (以下是原有的背景更新逻辑)
    if (m_bgItem && m_bgItem->pixmap().size() != img.size()) {
        m_bgItem->setPixmap(QPixmap::fromImage(img));
        m_scene->setSceneRect(img.rect());

        if (m_view) {
            QRectF rect = m_scene->sceneRect();
            if (rect.width() > 1 && rect.height() > 1) {
                m_view->fitInView(rect, Qt::KeepAspectRatio);
            }
        }
    } else if (m_bgItem) {
        m_bgItem->setPixmap(QPixmap::fromImage(img));
    }
    EventBus::instance()->sendUiFrameProcessed();
}

void MainWindow::setupTitleBar(QVBoxLayout* rootLayout) {
    m_titleBarWidget = new QWidget();
    m_titleBarWidget->setFixedHeight(40);
    m_titleBarWidget->setStyleSheet("background-color: #1f1f1f; border-bottom: 1px solid #333;");

    QHBoxLayout *header = new QHBoxLayout(m_titleBarWidget);
    header->setContentsMargins(15, 0, 5, 0);

    m_lblTitle = new QLabel("OBS Studio (Qt Enterprise Edition)");
    m_lblTitle->setStyleSheet("font-weight: bold; font-size: 13px; color: #ddd;");
    header->addWidget(m_lblTitle);
    header->addStretch();

    m_btnMin = new QPushButton("_");
    m_btnMin->setFixedSize(30, 30); m_btnMin->setObjectName("TitleBtn");
    connect(m_btnMin, &QPushButton::clicked, this, &FramelessWidget::onMinimizeRequested);
    header->addWidget(m_btnMin);

    m_btnMax = new QPushButton("□");
    m_btnMax->setFixedSize(30, 30); m_btnMax->setObjectName("TitleBtn");
    m_btnMax->setEnabled(m_config.windowMaximizable);
    connect(m_btnMax, &QPushButton::clicked, this, &FramelessWidget::onMaximizeRequested);
    header->addWidget(m_btnMax);

    m_btnClose = new QPushButton("✕");
    m_btnClose->setFixedSize(30, 30); m_btnClose->setObjectName("CloseBtn");
    connect(m_btnClose, &QPushButton::clicked, this, &FramelessWidget::onCloseRequested);
    header->addWidget(m_btnClose);

    rootLayout->addWidget(m_titleBarWidget);
}

void MainWindow::onBtnSettingsClicked() {
    SettingsDialog dlg(m_config, this);
    if (dlg.exec() == QDialog::Accepted) {
        m_config = dlg.getConfig();
        EventBus::instance()->sendCommandUpdateConfig(m_config);
        applyWindowConfig();
    }
}

void MainWindow::onBtnRefreshClicked() {
    this->refreshCameras();
    EventBus::instance()->sendCommandRefreshDevices();
}

void MainWindow::onBtnRecordClicked() { EventBus::instance()->sendCommandToggleRecording(!m_isRecording); }
void MainWindow::onChkMixMicToggled(bool checked) {
    m_comboMic->setEnabled(checked);
    m_progressMic->setEnabled(checked);
    EventBus::instance()->sendCommandToggleMicCapture(checked);
}
void MainWindow::onBtnAddCamClicked() {
    int index = m_comboCameras->currentIndex();
    if (index >= 0) EventBus::instance()->sendCommandAddCamera(m_comboCameras->itemData(index));
}
void MainWindow::onBtnSnapshotClicked() { EventBus::instance()->sendCommandSnapshot(); }
void MainWindow::onBtnLoadImageClicked() {
    QString path = QFileDialog::getOpenFileName(this, "Select Image", "", "Images (*.png *.jpg)");
    if (!path.isEmpty()) {
        QPixmap pix(path);
        if (!pix.isNull()) {
            int id = m_nextImageId++;
            ResizablePixmapItem *item = new ResizablePixmapItem(pix, id);
            item->setZValue(10); item->setPos(100, 100);
            m_scene->addItem(item); item->setSelected(true);
            m_overlayItems.insert(id, item);
            connect(item, &ResizablePixmapItem::geometryChanged, this, &MainWindow::onItemGeometryChanged);
            EventBus::instance()->sendOverlayGeometryChanged(id, QRect(100, 100, pix.width(), pix.height()));
            EventBus::instance()->fireOverlayUpdate(id, pix.toImage());
        }
    }
}
void MainWindow::onSourceComboChanged(int index) {
    if (index < 0) return;
    QList<QVariant> data = m_comboSources->itemData(index).value<QList<QVariant>>();
    if (data.size() == 2) EventBus::instance()->sendCommandChangeSource(data[0].toInt(), (size_t)data[1].toULongLong());
}
void MainWindow::onMicComboChanged(int index) {
    if (index < 0) return;
    EventBus::instance()->sendCommandChangeAudioDevice(true, m_comboMic->itemData(index).toString());
}
void MainWindow::onSysComboChanged(int index) {
    if (index < 0) return;
    EventBus::instance()->sendCommandChangeAudioDevice(false, m_comboSysAudio->itemData(index).toString());
}
void MainWindow::onItemGeometryChanged(int id, QRect rect) { EventBus::instance()->sendOverlayGeometryChanged(id, rect); }

void MainWindow::updateOverlay(int id, QImage img) {
    if (img.isNull() || img.width() == 0) return;
    if (!m_overlayItems.contains(id)) {
        QPixmap pix = QPixmap::fromImage(img);
        ResizablePixmapItem *item = new ResizablePixmapItem(pix, id);
        item->setZValue(20); item->setPos(50, 50);
        m_scene->addItem(item); item->setSelected(true);
        m_overlayItems.insert(id, item);
        connect(item, &ResizablePixmapItem::geometryChanged, this, &MainWindow::onItemGeometryChanged);
    } else {
        m_overlayItems[id]->setPixmap(QPixmap::fromImage(img));
    }
}
void MainWindow::updateAudioLevel(bool isMic, float level) {
    int val = static_cast<int>(level * 100);
    if (val > 100) val = 100;
    if (isMic) m_progressMic->setValue(val); else m_progressSys->setValue(val);
}
void MainWindow::onRecordingStateChanged(bool isRecording, QString msg) {
    m_isRecording = isRecording;
    if (isRecording) {
        m_btnRecord->setText("■ Stop Recording");
        m_btnRecord->setStyleSheet("background-color: #880000; color: white; font-weight: bold; border: 1px solid #aa0000;");
        m_btnSettings->setEnabled(false);
        m_lblDuration->setVisible(true); m_lblDuration->setText("00:00:00");
    } else {
        m_btnRecord->setText("● Start Recording");
        m_btnRecord->setStyleSheet("background-color: #2a2a2a; border: 1px solid #444; font-weight: bold;");
        m_btnSettings->setEnabled(true);
        if (!msg.isEmpty()) QMessageBox::information(this, "Stopped", "Saved to: " + msg);
    }
}
void MainWindow::onDurationUpdate(QString timeStr) { m_lblDuration->setText(timeStr); }
void MainWindow::onSourceListUpdated(QList<QPair<QString, QVariant>> sources) {
    m_comboSources->blockSignals(true); m_comboSources->clear();
    for(auto& pair : sources) m_comboSources->addItem(pair.first, pair.second);
    m_comboSources->blockSignals(false);
    if (m_comboSources->count() > 0) onSourceComboChanged(0);
}
void MainWindow::onAudioDeviceListUpdated(bool isMic, QStringList names, QStringList ids) {
    QComboBox* box = isMic ? m_comboMic : m_comboSysAudio;
    box->blockSignals(true); box->clear();
    for(int i=0; i<names.size(); ++i) box->addItem(names[i], ids[i]);
    box->blockSignals(false);
}
void MainWindow::onSnapshotTaken(QString path) { QMessageBox::information(this, "Snapshot", "Saved to: " + path); }
void MainWindow::onError(QString msg) { QMessageBox::critical(this, "Error", msg); }
