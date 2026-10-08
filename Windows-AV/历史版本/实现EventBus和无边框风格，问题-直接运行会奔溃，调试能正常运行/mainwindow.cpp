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

MainWindow::MainWindow(QWidget *parent) : FramelessWidget(parent)
{
    // 1. 初始尺寸与行为配置
    // 默认按照 config 里的 false, false
    setFixedSize(1060, 630);
    setTitleBarHeight(40);
    applyWindowConfig(); // 应用配置

    setupUi();

    // EventBus 连接
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

    // 延迟加载设备
    QTimer::singleShot(200, this, [this](){
        EventBus::instance()->sendCommandRefreshDevices();
    });
}

MainWindow::~MainWindow() {}

void MainWindow::applyWindowConfig() {
    setResizable(m_config.windowResizable);
    setMaximizable(m_config.windowMaximizable);
    if (m_btnMax) m_btnMax->setEnabled(m_config.windowMaximizable);
}

void MainWindow::setupUi() {
    // 设置高级深色主题
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
        /* 标题栏按钮特殊样式 */
        QPushButton#TitleBtn { background: transparent; border: none; }
        QPushButton#TitleBtn:hover { background: #444; }
        QPushButton#CloseBtn { background: transparent; border: none; }
        QPushButton#CloseBtn:hover { background: #e81123; }
    )");

    QVBoxLayout* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0); // 必须为0以实现无边框
    rootLayout->setSpacing(0);

    // 1. 自定义标题栏
    setupTitleBar(rootLayout);

    // 2. 内容容器
    QWidget* contentWidget = new QWidget();
    rootLayout->addWidget(contentWidget);

    QVBoxLayout* layout = new QVBoxLayout(contentWidget);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(10);

    // === 顶部：源选择 ===
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
    const QList<QCameraDevice> cameras = QMediaDevices::videoInputs();
    for (const QCameraDevice &c : cameras) m_comboCameras->addItem(c.description(), QVariant::fromValue(c));
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

    // === 中间：预览区域 ===
    m_scene = new QGraphicsScene(this);
    //m_scene->setSceneRect(0, 0, 1920, 1080);
    m_view = new QGraphicsView(m_scene);
    m_view->setRenderHint(QPainter::Antialiasing);
    m_view->setRenderHint(QPainter::SmoothPixmapTransform);
    m_view->setBackgroundBrush(Qt::black);
    m_view->setFrameShape(QFrame::NoFrame);
    // [新增] 禁用滚动条，实现由于 fitInView 控制的缩放
    m_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(m_view);

    m_bgItem = new QGraphicsPixmapItem();
    m_scene->addItem(m_bgItem);

    // === 底部：控制栏 ===
    QHBoxLayout *btnLayout = new QHBoxLayout();

    // [设置按钮]
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
    FramelessWidget::resizeEvent(event); // 调用基类处理（调整无边框逻辑）

    // 当窗口大小改变时，强制 View 重新适应 Scene 的内容
    if (m_view && m_scene && !m_scene->items().isEmpty()) {
        m_view->fitInView(m_scene->sceneRect(), Qt::KeepAspectRatio);
    }
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
    // 初始状态根据 config 决定
    m_btnMax->setEnabled(m_config.windowMaximizable);
    connect(m_btnMax, &QPushButton::clicked, this, &FramelessWidget::onMaximizeRequested);
    header->addWidget(m_btnMax);

    m_btnClose = new QPushButton("✕");
    m_btnClose->setFixedSize(30, 30); m_btnClose->setObjectName("CloseBtn");
    connect(m_btnClose, &QPushButton::clicked, this, &FramelessWidget::onCloseRequested);
    header->addWidget(m_btnClose);

    rootLayout->addWidget(m_titleBarWidget);
}

// [核心] 处理设置按钮
void MainWindow::onBtnSettingsClicked() {
    SettingsDialog dlg(m_config, this);
    if (dlg.exec() == QDialog::Accepted) {
        // 保存配置
        m_config = dlg.getConfig();
        EventBus::instance()->sendCommandUpdateConfig(m_config);

        // 立即应用窗口行为配置
        applyWindowConfig();
    }
}

// --- 其余业务逻辑 (保持不变) ---
void MainWindow::onBtnRecordClicked() { EventBus::instance()->sendCommandToggleRecording(!m_isRecording); }
void MainWindow::onChkMixMicToggled(bool checked) {
    m_comboMic->setEnabled(checked);
    m_progressMic->setEnabled(checked);
    EventBus::instance()->sendCommandToggleMicCapture(checked);
}
void MainWindow::onBtnRefreshClicked() { EventBus::instance()->sendCommandRefreshDevices(); }
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

// [修改] 预览更新函数：自动缩放
void MainWindow::updatePreview(QImage img) {
    // 1. 处理等比例缩放逻辑
    if (m_bgItem->pixmap().size() != img.size()) {
        m_bgItem->setPixmap(QPixmap::fromImage(img));
        m_scene->setSceneRect(img.rect());

        // 当源分辨率改变时，强制适应一次视图
        if (m_view) m_view->fitInView(m_bgItem, Qt::KeepAspectRatio);
    } else {
        m_bgItem->setPixmap(QPixmap::fromImage(img));
    }

    // 2. 【核心修复】告诉后台：这一帧UI已经画完了，请发下一帧！
    // 缺少这一行会导致画面卡死在第一帧或直接黑屏
    EventBus::instance()->sendUiFrameProcessed();
}

void MainWindow::updateOverlay(int id, QImage img) {
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
