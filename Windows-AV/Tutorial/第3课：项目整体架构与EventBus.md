# 第3课：项目整体架构与 EventBus

> **目标**：深刻理解整个系统的分层架构与模块通信机制，掌握 EventBus 事件总线的设计原理，能够独立追踪任意一条信号的完整调用链。  
> **对应文件**：`EventBus.h`、`SystemController.cpp`、`mainwindow.cpp`

---

## 1. 为什么需要架构设计？

在 `实现录屏/` 这个最初版本中，`MainWindow` 直接包含并调用了 `DxGiCapturer`、`FFmpegRecorder` 等采集模块。这种做法在功能简单时没有问题，但随着功能增加，很快就会出现：

- **紧耦合**：UI 代码和业务逻辑混在一起，改一处牵一发动全身
- **难以测试**：无法单独测试采集模块，必须启动整个 UI
- **线程混乱**：UI 线程直接调用耗时的采集操作，导致界面卡顿
- **崩溃频发**：多个模块互相直接依赖，生命周期难以管理

最终版本通过引入 **分层架构 + EventBus 事件总线**，彻底解决了上述问题。

---

## 2. 整体三层架构

项目采用经典的三层架构，层与层之间**只通过 EventBus 通信**，不直接互相调用：

```
┌─────────────────────────────────────────────────────────────────┐
│                         第一层：UI 层（主线程）                   │
│                                                                   │
│   MainWindow ──继承──> FramelessWidget                           │
│       │                                                           │
│       ├── QGraphicsView / QGraphicsScene（预览画面）              │
│       ├── ResizablePixmapItem（图层控件）                         │
│       ├── AddSourceDialog（添加源对话框）                         │
│       └── SettingsDialog（设置对话框）                            │
│                                                                   │
│   职责：显示画面、响应用户操作、发出命令信号                       │
└────────────────────────────┬────────────────────────────────────┘
                             │
                    ╔════════╧════════╗
                    ║   EventBus      ║  ← 唯一通信通道
                    ║  （全局单例）   ║
                    ╚════════╤════════╝
                             │
┌────────────────────────────┴────────────────────────────────────┐
│                      第二层：控制层（主线程）                     │
│                                                                   │
│   SystemController                                                │
│       │                                                           │
│       ├── 管理所有子模块的生命周期                                │
│       ├── 协调线程启停顺序                                        │
│       └── 连接各模块信号，路由数据                                │
│                                                                   │
│   职责：业务逻辑、状态管理、模块协调                              │
└──────┬──────────┬──────────┬──────────┬────────────────────────┘
       │          │          │          │
┌──────┴──┐ ┌────┴────┐ ┌───┴──────┐ ┌─┴────────┐
│  第三层：能力层（各自独立线程）                  │
│                                                  │
│DxGiCap- │ │FFmpeg   │ │WinRT     │ │WASAPI    │
│turer    │ │Recorder │ │Camera    │ │Capturer  │
│(视频线  │ │(编码线  │ │(摄像头   │ │(音频线   │
│ 程)     │ │ 程)     │ │ 线程)    │ │ 程)      │
└─────────┘ └─────────┘ └──────────┘ └──────────┘
     │              │
     ▼              ▼
 本地 MP4       RTMP 直播流
```

**三层的核心原则**：
- **UI 层**只能通过 `sendCommand_xxx()` 向下发命令，不直接调用任何采集/编码代码
- **控制层**通过 `fire_xxx()` 向上推送数据和状态，不直接操作 UI 控件
- **能力层**只关心自己的采集/编码逻辑，完全不知道 UI 的存在

---

## 3. EventBus 详解

### 3.1 单例实现

```cpp
// EventBus.h
class EventBus : public QObject {
    Q_OBJECT
public:
    static EventBus* instance() {
        static EventBus ins;  // C++11 线程安全的局部静态变量
        return &ins;
    }
    // ...
};
```

`EventBus` 是全局唯一实例，程序中任何地方都可以通过 `EventBus::instance()` 获取它。它继承自 `QObject`，因此可以使用 Qt 的信号槽机制。

### 3.2 信号分三类：cmd / data / state

这是 EventBus 最精妙的设计——信号按**语义**分为三类，通过命名前缀一眼区分方向和用途：

```
          UI 层                           控制层
            │                               │
            │ ── cmd_xxx ──────────────────>│  用户发出指令
            │                               │
            │ <──── data_xxx ───────────────│  持续推送的数据流
            │                               │
            │ <──── state_xxx ──────────────│  一次性的状态变化
```

| 前缀 | 方向 | 含义 | 示例 |
|------|------|------|------|
| `cmd_` | UI → Controller | 用户指令，要求 Controller 执行某个动作 | `cmd_toggleRecording`、`cmd_addCamera` |
| `data_` | Controller → UI | 持续流式推送的数据，高频 | `data_previewFrame`（每帧画面）、`data_audioLevel`（音量电平） |
| `state_` | Controller → UI | 状态变更通知，低频 | `state_recordingChanged`、`state_cameraAdded` |

### 3.3 包装方法：sendCommand / fire

EventBus 在信号之外，还提供了一层包装方法，使调用更语义化：

```cpp
// EventBus.h

// UI 层调用（发出命令）
void sendCommandToggleRecording(bool start) {
    emit cmd_toggleRecording(start);  // 内部直接 emit 信号
}

// Controller 层调用（推送数据/状态）
void firePreviewFrame(const QImage& img) {
    emit data_previewFrame(img);
}
void fireRecordingStateChanged(bool isRecording, const QString& msg) {
    emit state_recordingChanged(isRecording, msg);
}
```

**为什么要包装？** 直接 `emit` 需要知道信号名，而 `sendCommandToggleRecording` 这样的命名更清晰，IDE 自动补全也更友好。

---

## 4. 完整信号清单

### 4.1 cmd 类（UI → Controller）

| 信号 | 触发时机 | 处理者 |
|------|----------|--------|
| `cmd_toggleRecording(bool)` | 用户点击录制/停止按钮 | `onToggleRecording` |
| `cmd_changeSource(int, size_t)` | 用户切换采集源（显示器/窗口） | `onChangeSource` |
| `cmd_addCamera(QVariant)` | 用户点击添加摄像头 | `onAddCamera` |
| `cmd_updateConfig(RecorderConfig)` | 用户保存设置 | `onUpdateConfig` |
| `cmd_refreshDevices()` | 用户点击刷新设备 | `onRefreshDevices` |
| `cmd_takeSnapshot()` | 用户点击截图 | `onTakeSnapshot` |
| `cmd_changeAudioDevice(bool, QString)` | 用户切换音频设备 | `onChangeAudioDevice` |
| `cmd_toggleMicCapture(bool)` | 用户开关麦克风混音 | `onToggleMicCapture` |
| `cmd_removeOverlay(int)` | 用户删除图层 | Lambda |
| `cmd_updateRenderOrder(QList<int>)` | 用户拖动图层顺序 | Lambda |
| `cmd_uiFrameProcessed()` | UI 渲染完一帧通知 | `onUiFrameProcessed` |
| `cmd_overlayGeometryChanged(int, QRect)` | 用户拖动/缩放图层 | `onOverlayGeometryChanged` |

### 4.2 data 类（Controller → UI，高频）

| 信号 | 触发频率 | 用途 |
|------|----------|------|
| `data_previewFrame(QImage)` | 30fps | 更新预览画面 |
| `data_overlayUpdate(int, QImage)` | 30fps | 更新摄像头图层画面 |
| `data_audioLevel(bool, float)` | 高频 | 更新音量电平进度条 |

### 4.3 state 类（Controller → UI，低频）

| 信号 | 触发时机 | 用途 |
|------|----------|------|
| `state_recordingChanged(bool, QString)` | 录制开始/停止时 | 更新录制按钮状态 |
| `state_durationUpdated(QString)` | 每秒一次（录制中） | 更新录制时长显示 |
| `state_sourceListUpdated(...)` | 刷新设备后 | 更新采集源下拉列表 |
| `state_audioDeviceListUpdated(...)` | 刷新设备后 | 更新音频设备下拉列表 |
| `state_cameraAdded(int, QRect)` | 摄像头添加成功后 | 在场景中创建摄像头图层控件 |
| `state_snapshotTaken(QString)` | 截图完成后 | 显示截图保存路径提示 |
| `state_error(QString)` | 任何错误发生时 | 显示错误对话框 |

---

## 5. SystemController：系统的大脑

`SystemController` 是控制层的核心，它在 EventBus 上连接所有信号，协调整个系统的运转。

### 5.1 初始化：订阅所有命令信号

```cpp
// SystemController.cpp
void SystemController::initialize() {
    EventBus* bus = EventBus::instance();

    // 订阅所有来自 UI 的命令信号
    connect(bus, &EventBus::cmd_changeSource,     this, &SystemController::onChangeSource);
    connect(bus, &EventBus::cmd_addCamera,        this, &SystemController::onAddCamera);
    connect(bus, &EventBus::cmd_toggleRecording,  this, &SystemController::onToggleRecording);
    connect(bus, &EventBus::cmd_updateConfig,     this, &SystemController::onUpdateConfig);
    connect(bus, &EventBus::cmd_refreshDevices,   this, &SystemController::onRefreshDevices);
    // ... 共 10+ 个连接

    startVideoThread();  // 启动视频采集线程
}
```

注意：这里没有指定 `Qt::QueuedConnection`，使用默认的 `AutoConnection`。由于 `EventBus` 和 `SystemController` 都在主线程，会自动使用直接连接（`DirectConnection`）。

### 5.2 启动视频线程

```cpp
void SystemController::startVideoThread() {
    m_videoThread = new QThread(this);
    m_capturer = new DxGiCapturer();
    m_capturer->moveToThread(m_videoThread);  // 将捕获器移到视频线程

    // 线程启动后，在视频线程中初始化 D3D 环境
    connect(m_videoThread, &QThread::started, m_capturer, [this]() {
        RoInitialize(RO_INIT_MULTITHREADED);  // 初始化 WinRT 运行时
        if (m_capturer->init()) {
            m_capturer->startCapture();
        } else {
            EventBus::instance()->fireError("Failed to initialize DXGI Capturer");
        }
    });

    // 【关键】采集帧发出 → 回到主线程 → 推给编码器
    connect(m_capturer, &DxGiCapturer::frameCaptured,
            this, &SystemController::onVideoFrameCaptured,
            Qt::QueuedConnection);  // 视频线程 → 主线程，必须 Queued

    // 预览帧直接发给 EventBus，UI 层监听
    connect(m_capturer, &DxGiCapturer::frameCapturedForPreview,
            EventBus::instance(), &EventBus::data_previewFrame,
            Qt::QueuedConnection);

    m_videoThread->start();
}
```

### 5.3 处理录制开关

```cpp
void SystemController::onToggleRecording(bool start) {
    if (start) {
        // 生成默认文件名（如果用户没有设置）
        if (m_config.savePath.isEmpty())
            m_config.savePath = "Rec_" + QDateTime::currentDateTime()
                                              .toString("yyyyMMdd_HHmmss") + ".mp4";

        // 通知采集器开始录制帧（仅打标记，不影响预览帧）
        QMetaObject::invokeMethod(m_capturer, "startRecording", Qt::QueuedConnection);

        // 启动 FFmpeg 编码器
        if (m_recorder->startRecord(m_config)) {
            m_recordElapsed.restart();
            m_durationTimer->start();
            EventBus::instance()->fireRecordingStateChanged(true, m_config.savePath);
        }
    } else {
        m_recorder->stopRecord();
        QMetaObject::invokeMethod(m_capturer, "stopRecording", Qt::QueuedConnection);
        m_durationTimer->stop();
        EventBus::instance()->fireRecordingStateChanged(false, "");
        EventBus::instance()->fireDurationUpdate("00:00:00");
    }
}
```

这个函数是 UI 层和能力层之间的**翻译器**：
- 收到 `cmd_toggleRecording(true)` → 启动编码器 + 通知采集器开始存帧
- 把结果通过 `state_recordingChanged` 反馈给 UI

### 5.4 析构：有序地清理所有资源

```cpp
SystemController::~SystemController() {
    // 停止顺序非常重要！先停上游，再停下游
    if (m_recorder) {
        m_recorder->stopRecord();
        m_recorder->wait();     // 等待编码线程结束
        delete m_recorder;
    }

    stopVideoThread();          // 停采集线程
    cleanupCameraPool();        // 清理摄像头资源池

    if (m_micCapturer) {
        m_micCapturer->stop();
        m_micCapturer->wait();  // 等待音频线程结束
        delete m_micCapturer;
    }
    if (m_sysCapturer) {
        m_sysCapturer->stop();
        m_sysCapturer->wait();
        delete m_sysCapturer;
    }
}
```

**停止顺序的逻辑**：
1. 先停 `FFmpegRecorder`（编码器），防止停采集后还有帧推入已销毁的编码器
2. 再停 `DxGiCapturer`（视频采集），此时不会再有新帧产生
3. 最后停 `WasapiCapturer`（音频采集）

顺序弄反会导致：采集线程还在 `push` 帧，编码对象已被 `delete` → 崩溃。

---

## 6. UI 层如何使用 EventBus

```cpp
// mainwindow.cpp - 构造函数中订阅所有推送信号
EventBus* bus = EventBus::instance();
connect(bus, &EventBus::data_previewFrame,          this, &MainWindow::updatePreview);
connect(bus, &EventBus::data_audioLevel,            this, &MainWindow::updateAudioLevel);
connect(bus, &EventBus::state_recordingChanged,     this, &MainWindow::onRecordingStateChanged);
connect(bus, &EventBus::state_durationUpdated,      this, &MainWindow::onDurationUpdate);
connect(bus, &EventBus::state_sourceListUpdated,    this, &MainWindow::onSourceListUpdated);
connect(bus, &EventBus::state_audioDeviceListUpdated, this, &MainWindow::onAudioDeviceListUpdated);
connect(bus, &EventBus::state_cameraAdded,          this, &MainWindow::onCameraAdded);

// 用户点击录制按钮 → 只发命令，不关心底层怎么实现
void MainWindow::onBtnRecordClicked() {
    EventBus::instance()->sendCommandToggleRecording(!m_isRecording);
}

// 用户切换采集源 → 打包参数发出命令
void MainWindow::onSourceComboChanged(int index) {
    QList<QVariant> data = m_comboSources->itemData(index).value<QList<QVariant>>();
    if (data.size() == 2)
        EventBus::instance()->sendCommandChangeSource(data[0].toInt(), (size_t)data[1].toULongLong());
}
```

UI 层的特点：
- **只发命令，不关心结果**：`sendCommandToggleRecording()` 发出去就完了，不等待返回值
- **只被动接收推送**：`updatePreview`、`onRecordingStateChanged` 等都是被动响应
- **完全不知道** `DxGiCapturer`、`FFmpegRecorder` 的存在，也不 `#include` 它们

---

## 7. 完整调用链追踪：用户点击"开始录制"

用这个例子把整个架构串起来：

```
【第一步：UI 层 - 主线程】
用户点击录制按钮
    │
    ▼
MainWindow::onBtnRecordClicked()
    │  EventBus::instance()->sendCommandToggleRecording(true)
    │  └─ emit cmd_toggleRecording(true)
    │
    ▼ （AutoConnection，同线程直接调用）

【第二步：控制层 - 主线程】
SystemController::onToggleRecording(true)
    │
    ├─① QMetaObject::invokeMethod(m_capturer, "startRecording", QueuedConnection)
    │    └── 投递到视频线程事件队列
    │
    └─② m_recorder->startRecord(m_config)
         └── FFmpegRecorder 启动编码线程 run()

    之后：emit state_recordingChanged(true, "Rec_xxx.mp4")
              └─ MainWindow::onRecordingStateChanged(true, ...)
                      └─ 按钮变红、显示文件名

【第三步（并行）：视频线程】
DxGiCapturer::startRecording()  ← 收到 invokeMethod 的投递
    └── m_isRecording = true（标记开始录制帧，之前只预览不录）

    采集循环（每 33ms）：
    AcquireNextFrame() → 合成 GPU 帧 → QImage
        │
        ├─ emit frameCapturedForPreview(previewImg)
        │    └─ EventBus::data_previewFrame → MainWindow::updatePreview()（UI更新预览）
        │
        └─ emit frameCaptured(recordImg, timestamp)  ← 仅 m_isRecording=true 时
             └─ SystemController::onVideoFrameCaptured()  （QueuedConnection，回到主线程）
                  └─ m_recorder->pushVideoFrame(img, ts)
                       └─ 加锁放入 m_videoQueue

【第四步：编码线程】
FFmpegRecorder::run() 循环：
    从 m_videoQueue 取帧
        │
        ├─ sws_scale()（BGRA → YUV420P）
        ├─ avcodec_send_frame()
        ├─ avcodec_receive_packet()
        └─ av_interleaved_write_frame() → 写入 MP4 文件
```

---

## 8. EventBus 与直接调用的对比

### 8.1 没有 EventBus 时（v1 版本风格）

```cpp
// MainWindow 直接持有所有模块的引用 ← 高耦合
class MainWindow {
    DxGiCapturer* m_capturer;
    FFmpegRecorder* m_recorder;
    WasapiCapturer* m_micCapturer;

    void onBtnRecordClicked() {
        m_capturer->startRecording();   // 直接调用
        m_recorder->startRecord(cfg);   // 直接调用
    }
};
```

**问题**：
- MainWindow 必须 `#include` 所有模块的头文件
- 采集模块的接口变化，UI 代码也得改
- 无法在不启动 UI 的情况下测试采集模块

### 8.2 有 EventBus 后（最终版本）

```cpp
// MainWindow 只知道 EventBus ← 低耦合
class MainWindow {
    void onBtnRecordClicked() {
        EventBus::instance()->sendCommandToggleRecording(true);  // 只发命令
    }
};

// SystemController 只知道 EventBus + 各模块 ← 单向依赖
class SystemController {
    DxGiCapturer* m_capturer;
    FFmpegRecorder* m_recorder;
    // SystemController 知道底层，但 UI 不需要知道 SystemController 的存在
};
```

**收益**：

| 特性 | 无 EventBus | 有 EventBus |
|------|------------|-------------|
| 模块间依赖 | 网状（互相引用） | 星状（都只依赖 EventBus） |
| 线程安全 | 需要每处都小心处理 | EventBus 信号自动处理跨线程 |
| 可测试性 | 很难单独测试 | 可以单独发信号驱动测试 |
| 新增功能 | 需要修改多个类 | 只需在 EventBus 加信号，订阅者各自连接 |
| 代码可读性 | 调用链复杂，难以追踪 | 信号名语义清晰，一眼知道用途 |

---

## 9. EventBus 在项目历史中的诞生

打开 `实现EventBus和无边框风格，问题-直接运行会奔溃，调试能正常运行/EventBus.h`，与当前版本对比：

**早期版本**（刚引入 EventBus 时）：
- 信号数量少（约 15 个）
- 还没有 `cmd_removeOverlay`、`cmd_updateRenderOrder`
- 还没有 `state_cameraAdded`

**当前版本**：
- 信号增至 20+ 个
- 每次新增功能（RTMP 推流、摄像头图层、WGC 采集）都只需在 EventBus 加信号，不需要改动已有的 UI 代码

这个演进过程说明：**好的架构是可以持续扩展的，每次新增功能的"手术范围"都很小**。

---

## 10. 本课作业

1. **全信号地图**：以 EventBus.h 为蓝本，画一张完整的信号流向图，标注每个信号的发送方和接收方（可参考本课第 4 节的信号清单）。

**答案：**

```text
一、cmd 类（UI → Controller，发送方：MainWindow/EventBus，接收方：SystemController）

MainWindow
  └─ EventBus::sendCommandChangeSource(...)
      └─ cmd_changeSource(int, size_t)
          └─ SystemController::onChangeSource

MainWindow
  └─ EventBus::sendCommandAddCamera(...)
      └─ cmd_addCamera(QVariant)
          └─ SystemController::onAddCamera

MainWindow
  └─ EventBus::sendCommandToggleRecording(...)
      └─ cmd_toggleRecording(bool)
          └─ SystemController::onToggleRecording

MainWindow / SettingsDialog
  └─ EventBus::sendCommandUpdateConfig(...)
      └─ cmd_updateConfig(RecorderConfig)
          └─ SystemController::onUpdateConfig

MainWindow
  └─ EventBus::sendCommandRefreshDevices()
      └─ cmd_refreshDevices()
          └─ SystemController::onRefreshDevices

MainWindow
  └─ EventBus::sendCommandSnapshot()
      └─ cmd_takeSnapshot()
          └─ SystemController::onTakeSnapshot

MainWindow
  └─ EventBus::sendCommandChangeAudioDevice(...)
      └─ cmd_changeAudioDevice(bool, QString)
          └─ SystemController::onChangeAudioDevice

MainWindow
  └─ EventBus::sendCommandToggleMicCapture(bool)
      └─ cmd_toggleMicCapture(bool)
          └─ SystemController::onToggleMicCapture

MainWindow
  └─ EventBus::sendUiFrameProcessed()
      └─ cmd_uiFrameProcessed()
          └─ SystemController::onUiFrameProcessed

ResizablePixmapItem / MainWindow
  └─ EventBus::sendOverlayGeometryChanged(...)
      └─ cmd_overlayGeometryChanged(int, QRect)
          └─ SystemController::onOverlayGeometryChanged
          └─ 同时在 startVideoThread 中也直接连接到 DxGiCapturer::updateOverlayGeometry（QueuedConnection）

MainWindow
  └─ EventBus::sendCommandRemoveOverlay(int)
      └─ cmd_removeOverlay(int)
          └─ SystemController 内 lambda 处理

MainWindow
  └─ EventBus::sendCommandUpdateRenderOrder(QList<int>)
      └─ cmd_updateRenderOrder(QList<int>)
          └─ SystemController 内 lambda 处理

二、data 类（Controller/能力层 → UI）

DxGiCapturer（视频线程）
  └─ frameCapturedForPreview(QImage)
      └─ EventBus::data_previewFrame(QImage)
          └─ MainWindow::updatePreview

EventBus / SystemController / 摄像头图层逻辑
  └─ data_overlayUpdate(int, QImage)
      ├─ MainWindow::updateOverlay
      └─ DxGiCapturer::updateOverlayImage（QueuedConnection，发回渲染线程）

SystemController
  └─ EventBus::fireAudioLevel(bool, float)
      └─ data_audioLevel(bool, float)
          └─ MainWindow::updateAudioLevel

三、state 类（Controller → UI）

SystemController
  └─ EventBus::fireRecordingStateChanged(bool, QString)
      └─ state_recordingChanged(bool, QString)
          └─ MainWindow::onRecordingStateChanged

SystemController
  └─ EventBus::fireDurationUpdate(QString)
      └─ state_durationUpdated(QString)
          └─ MainWindow::onDurationUpdate

SystemController
  └─ EventBus::fireSourceListUpdated(...)
      └─ state_sourceListUpdated(...)
          └─ MainWindow::onSourceListUpdated

SystemController
  └─ EventBus::fireAudioDeviceListUpdated(...)
      └─ state_audioDeviceListUpdated(...)
          └─ MainWindow::onAudioDeviceListUpdated

SystemController
  └─ EventBus::fireSnapshotTaken(QString)
      └─ state_snapshotTaken(QString)
          └─ MainWindow::onSnapshotTaken

SystemController
  └─ EventBus::fireError(QString)
      └─ state_error(QString)
          └─ MainWindow::onError

SystemController
  └─ EventBus::fireCameraAdded(int, QRect)
      └─ state_cameraAdded(int, QRect)
          └─ MainWindow::onCameraAdded
```

2. **调用链追踪**：参考第 7 节的分析方式，自己追踪 **"用户点击截图"** 的完整调用链（提示：从 `onBtnSnapshotClicked` 开始，到 `state_snapshotTaken` 结束），写出每一步发生在哪个线程。

**答案：**

```text
【第一步：UI 层 - 主线程】
用户点击 Snapshot 按钮
    │
    ▼
MainWindow::onBtnSnapshotClicked()
    │  EventBus::instance()->sendCommandSnapshot()
    │  └─ emit cmd_takeSnapshot()
    ▼

【第二步：控制层 - 主线程】
SystemController::onTakeSnapshot()
    │
    └─ m_capturer->requestSnapshot()
         这里是主线程直接调用；本质作用是把
         DxGiCapturer::m_snapshotRequested 置为 1

【第三步：能力层 - 视频采集线程】
DxGiCapturer::startCapture() 的采集循环继续运行
    │
    └─ 当下一帧采集完成后，检测到
       m_snapshotRequested.exchange(0) == 1
          │
          └─ emit snapshotCaptured(deepCopy)

【第四步：控制层 - 主线程】
由于在 SystemController::startVideoThread() 中：
connect(m_capturer,
        &DxGiCapturer::snapshotCaptured,
        this,
        &SystemController::onSnapshotCaptured,
        Qt::QueuedConnection)

所以该信号会从视频线程投递回主线程：
    │
    ▼
SystemController::onSnapshotCaptured(const QImage& img)
    │
    ├─ QString f = "Snap.png"
    ├─ img.save(f)
    └─ EventBus::instance()->fireSnapshotTaken(f)
         └─ emit state_snapshotTaken(f)

【第五步：UI 层 - 主线程】
MainWindow 在构造函数中订阅：
connect(bus, &EventBus::state_snapshotTaken,
        this, &MainWindow::onSnapshotTaken)

因此：
state_snapshotTaken(QString)
    └─ MainWindow::onSnapshotTaken(QString path)
         └─ QMessageBox::information(this, "Snap", "Saved: " + path)
```

**线程总结：**
- `MainWindow::onBtnSnapshotClicked`：主线程
- `SystemController::onTakeSnapshot`：主线程
- `DxGiCapturer` 检测截图请求并 `emit snapshotCaptured`：视频采集线程
- `SystemController::onSnapshotCaptured`：主线程（因为用了 `Qt::QueuedConnection`）
- `state_snapshotTaken` 和 `MainWindow::onSnapshotTaken`：主线程

3. **新增信号练习**：假设要新增一个功能——"录制完成后显示文件大小"，请描述：  
   - 需要在 EventBus 中新增哪个信号（命名、参数类型、属于哪一类）？  
   - `FFmpegRecorder` 在哪里 emit 这个信号？  
   - `SystemController` 需要做什么中转？  
   - `MainWindow` 在哪里连接并显示文件大小？

**答案：**

我会这样设计：

- **EventBus 新增信号**：`state_fileSizeReady(QString path, qint64 bytes)`
- **信号类别**：属于 **state 类**，因为“录制完成后文件大小已确定”是一次性的状态通知，不是持续高频数据流。
- **参数含义**：
  - `QString path`：录制文件路径，方便 UI 显示是哪一个文件
  - `qint64 bytes`：文件大小，单位字节

可在 `EventBus.h` 中补两处：

```cpp
void fireFileSizeReady(const QString& path, qint64 bytes) {
    emit state_fileSizeReady(path, bytes);
}

signals:
    void state_fileSizeReady(QString path, qint64 bytes);
```

- **`FFmpegRecorder` 在哪里 emit？**
  最合适的位置是录制线程真正写完文件尾并关闭文件之后，也就是 `FFmpegRecorder::run()` 末尾、`av_write_trailer(m_fmtCtxFile)` 执行完成之后、`closeResources()` 之前或之后都可以，但必须保证文件已经完整落盘。此时可以用 `QFileInfo(m_config.savePath).size()` 读取最终大小。

例如可以给 `FFmpegRecorder` 自己新增一个信号：

```cpp
signals:
    void fileSizeReady(QString path, qint64 bytes);
```

然后在 `run()` 收尾阶段发出：

```cpp
if (m_fmtCtxFile) {
    av_write_trailer(m_fmtCtxFile);
    QFileInfo info(m_config.savePath);
    emit fileSizeReady(m_config.savePath, info.size());
}
```

- **`SystemController` 需要做什么中转？**
  `SystemController` 负责把能力层的信号转成 EventBus 的 state 信号。

  也就是在构造或初始化时连接：

```cpp
connect(m_recorder,
        &FFmpegRecorder::fileSizeReady,
        this,
        [=](const QString& path, qint64 bytes) {
            EventBus::instance()->fireFileSizeReady(path, bytes);
        },
        Qt::QueuedConnection);
```

  这样流程仍然符合“能力层不直接碰 UI、UI 只认 EventBus”的架构原则。

- **`MainWindow` 在哪里连接并显示文件大小？**
  在 `MainWindow` 构造函数里，和其他 `state_` 信号一起连接：

```cpp
connect(bus,
        &EventBus::state_fileSizeReady,
        this,
        &MainWindow::onFileSizeReady);
```

  然后新增槽函数，例如：

```cpp
void MainWindow::onFileSizeReady(QString path, qint64 bytes)
{
    double mb = bytes / 1024.0 / 1024.0;
    QMessageBox::information(this,
                             "录制完成",
                             QString("文件：%1\n大小：%2 MB")
                                 .arg(path)
                                 .arg(mb, 0, 'f', 2));
}
```

4. **思考题**：本项目的 EventBus 是一个全局单例。这种设计有什么缺点？在什么情况下，一个项目不适合使用全局单例 EventBus？

**答案：**

全局单例 EventBus 的优点是接线方便、解耦明显，但它也有几个典型缺点：

1. **全局状态过强，依赖不显式**  
   任意模块都能拿到 `EventBus::instance()` 发信号，表面上看模块之间不直接依赖了，但实际上都偷偷依赖同一个全局对象。阅读代码时，不容易一眼看出“谁会影响谁”。

2. **调用链容易分散，调试成本上升**  
   尤其当信号越来越多时，一个按钮点下去到底是谁处理、谁又继续转发，需要不断跳转搜索，复杂项目里会比普通函数调用更难追踪。

3. **名字空间容易膨胀**  
   所有事件都堆在一个 `EventBus.h` 里，后期可能出现几十上百个信号，文件会越来越大，维护成本上升，也容易出现命名混乱。

4. **测试不够友好**  
   因为它是单例，单元测试里很难为不同测试用例创建不同总线实例，也不方便隔离状态；如果测试并发运行，还可能互相影响。

5. **模块边界会被稀释**  
   如果团队滥用 EventBus，任何事都靠发事件解决，就会让本来应该清晰的接口设计消失，最后演变成“大家都能往总线上喊一句”，架构反而变得隐式耦合。

6. **不利于大型团队协作**  
   当多人同时开发时，大家都往同一个全局 EventBus 加信号，容易冲突，也容易出现“一个事件被多个地方偷偷监听”的问题。

**什么情况下不适合用全局单例 EventBus？**

- **模块边界要求非常严格的项目**：比如大型业务系统、插件化系统、分层非常清晰的架构，更适合显式依赖注入，而不是全局单例。
- **需要高可测性的项目**：如果项目非常强调单元测试、mock、替身对象，那么传入接口对象通常比依赖全局单例更好测。
- **多实例场景**：如果将来一个进程里可能同时创建多个“录制会话”或多个独立工作区，共用一个全局 EventBus 就会互相串线，这时应该为每个实例配自己的消息总线。
- **事件量极大的系统**：如果事件特别多、来源特别复杂，全局总线会变成“上帝对象”，这时更适合按领域拆分成多个局部 Bus，或者改用明确的服务接口。

**总结一句话：**
全局单例 EventBus 适合当前这种单窗口、单控制中心、模块数量可控的桌面项目；但当项目走向“大型化、多实例化、强测试化”时，它就会逐渐从优点变成负担。

---

*上一课：[第2课：C++20 与 Qt6 核心基础](./第2课：C++20与Qt6核心基础.md)*  
*下一课：[第4课：SystemController 与多线程管理](./第4课：SystemController与多线程管理.md)*
