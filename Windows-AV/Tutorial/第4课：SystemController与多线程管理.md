# 第4课：SystemController 与多线程管理

> **目标**：深入理解 SystemController 如何协调多个线程，掌握生产者-消费者模式、摄像头资源池、安全停止线程等工程实践。  
> **对应文件**：`SystemController.h/cpp`、`FFmpegRecorder.cpp`（run 方法）、`WasapiCapturer.cpp`（run 方法）

---

## 1. 项目的线程全景图

在深入 SystemController 之前，先建立整个系统的线程视图：

```
┌────────────────────────────────────────────────────────────────┐
│                      主线程（Qt 事件循环）                       │
│                                                                  │
│  QApplication::exec()                                           │
│       │                                                          │
│       ├── MainWindow（UI 渲染、用户交互）                        │
│       ├── SystemController（协调器、状态机）                     │
│       ├── QTimer::timeout（每秒触发 onDurationTimer）            │
│       └── EventBus 信号的 AutoConnection 槽                      │
└──────────────────────────────────────────────────────────────┬─┘
                                                               │
        ┌──────────────────────────────────────────────────────┤
        │                                                      │
┌───────┴──────────┐  ┌──────────────────┐  ┌────────────────┴──┐
│  视频线程         │  │  编码线程         │  │  音频线程(×2)      │
│  (m_videoThread) │  │  (FFmpegRecorder)│  │  (WasapiCapturer) │
│                  │  │                  │  │                    │
│ DxGiCapturer     │  │ FFmpegRecorder   │  │ m_micCapturer     │
│ - init()         │  │ - run()          │  │ m_sysCapturer     │
│ - startCapture() │  │   编码主循环     │  │ - run()           │
│ - WgcCapturer    │  │   音视频混流     │  │   WASAPI 采集循环 │
│ - WinRTCamera    │  │   写 MP4/RTMP   │  │   PCM 数据推送    │
│                  │  │                  │  │                    │
│ moveToThread()   │  │ 继承 QThread     │  │ 继承 QThread      │
│ 方式             │  │ 方式             │  │ 方式              │
└────────┬─────────┘  └────────┬─────────┘  └─────────┬─────────┘
         │                     │                       │
         │  QueuedConnection   │  QMutex+QQueue        │  Lambda slot
         │  (跨线程信号)       │  (跨线程数据)         │  (跨线程信号)
         └──────────────────── → 主线程 SystemController ←──────┘
```

**三个关键约束**：
1. **UI 控件只能在主线程访问**：任何从非主线程直接操作 QWidget 的行为都会导致崩溃
2. **D3D 设备只能在创建它的线程使用**：D3D11 设备在视频线程中创建，所有 GPU 操作必须在视频线程进行
3. **COM 的 STA/MTA 模型**：WinRT（STA）和 WASAPI（STA）需要在各自线程初始化 COM 环境

---

## 2. 成员变量解析

```cpp
// SystemController.h
class SystemController : public QObject {
private:
    // ── 线程与核心模块 ──
    QThread*         m_videoThread  = nullptr;  // 视频采集线程对象
    DxGiCapturer*    m_capturer     = nullptr;  // 运行在 m_videoThread 中
    FFmpegRecorder*  m_recorder     = nullptr;  // 自身就是线程（继承 QThread）
    WasapiCapturer*  m_micCapturer  = nullptr;  // 麦克风采集线程
    WasapiCapturer*  m_sysCapturer  = nullptr;  // 系统声音采集线程

    // ── 摄像头资源池 ──
    QList<CameraContext*>      m_activeLayers;  // 当前活跃的图层列表
    QMap<QString, WinRTCamera*> m_cameraPool;  // 预热的摄像头实例池
    QMutex                     m_poolMutex;    // 保护以上两个容器

    // ── 配置与计时 ──
    int              m_nextItemId   = 100;      // 图层 ID 自增计数器
    RecorderConfig   m_config;                  // 当前录制配置
    QTimer*          m_durationTimer;           // 每秒触发，更新录制时长
    QElapsedTimer    m_recordElapsed;           // 高精度计时器

    // ── 音频状态 ──
    QString          m_currentMicId;            // 当前选中的麦克风 ID
    QString          m_currentSysId;            // 当前选中的系统音频 ID
    bool             m_isMicEnabled = false;    // 麦克风混音开关
};
```

**设计亮点**：所有成员指针都初始化为 `nullptr`，析构时可以安全地判断 `if (ptr)` 再操作。

---

## 3. 构造函数：只做最基础的初始化

```cpp
SystemController::SystemController(QObject *parent) : QObject(parent) {
    m_recorder = new FFmpegRecorder(nullptr);   // 创建编码器（但还未启动）
    m_durationTimer = new QTimer(this);
    m_durationTimer->setInterval(1000);         // 每秒触发一次
    connect(m_durationTimer, &QTimer::timeout,
            this, &SystemController::onDurationTimer);
    LOG_INFO("SystemController Created.");
}
```

注意：`FFmpegRecorder` 在构造函数里就创建了，但 `start()` 还没有调用——它只是一个对象，不是一个线程。线程是在 `startRecord()` 时才真正启动的。

---

## 4. 视频线程的启动与停止

### 4.1 startVideoThread：moveToThread 模式

```cpp
void SystemController::startVideoThread() {
    if (m_videoThread) return;   // 防止重复启动

    m_videoThread = new QThread(this);
    m_capturer    = new DxGiCapturer();
    m_capturer->moveToThread(m_videoThread);  // 关键：将捕获器"移居"到视频线程
```

`moveToThread` 之后，`m_capturer` 的所有**槽函数**都会在 `m_videoThread` 中执行。这意味着：
- `m_capturer->updateOverlayImage(...)` 这个槽 → 在视频线程执行 ✅
- `m_capturer->frameCaptured` 这个信号 → 由视频线程发出 ✅

```cpp
    // 线程启动后，在视频线程中初始化 COM/WinRT 运行时和 D3D 设备
    connect(m_videoThread, &QThread::started, m_capturer, [this]() {
        // RoInitialize：WinRT 运行时初始化（多线程模式）
        HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
        if (FAILED(hr))
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // 回退到 COM 初始化

        if (m_capturer->init()) {     // 初始化 D3D11 设备、DXGI
            m_capturer->startCapture();
        } else {
            EventBus::instance()->fireError("Failed to initialize DXGI Capturer");
        }
    });
```

**为什么要在线程启动后才初始化 D3D？**  
D3D11 设备与创建它的线程绑定。如果在主线程创建设备，视频线程就无法使用它。通过 `QThread::started` 信号，确保初始化代码在视频线程的上下文中执行。

```cpp
    // 建立跨线程连接
    connect(m_capturer, &DxGiCapturer::frameCaptured,
            this, &SystemController::onVideoFrameCaptured,
            Qt::QueuedConnection);   // 视频线程 → 主线程

    connect(m_capturer, &DxGiCapturer::frameCapturedForPreview,
            EventBus::instance(), &EventBus::data_previewFrame,
            Qt::QueuedConnection);   // 视频线程 → 主线程 → UI

    // 线程结束时自动销毁捕获器
    connect(m_videoThread, &QThread::finished,
            m_capturer, &QObject::deleteLater);  // 在视频线程中销毁（安全）
    connect(m_videoThread, &QThread::finished,
            this, []() { RoUninitialize(); });    // 清理 WinRT 运行时

    m_videoThread->start();  // 启动线程，触发 started 信号
}
```

**`deleteLater` vs `delete`**：
- `delete m_capturer`：立即销毁，如果此时视频线程还在执行采集循环 → 崩溃
- `m_capturer->deleteLater()`：将销毁操作投递到 `m_capturer` 所在线程（视频线程）的事件队列，等当前任务执行完毕后安全销毁

### 4.2 stopVideoThread：阻塞等待

```cpp
void SystemController::stopVideoThread() {
    // 第一步：让视频线程中的捕获循环停下来
    if (m_capturer)
        QMetaObject::invokeMethod(m_capturer, "stopCapture",
                                  Qt::BlockingQueuedConnection);
    // BlockingQueuedConnection：主线程在这里阻塞，直到视频线程执行完 stopCapture()

    // 第二步：退出视频线程的事件循环
    if (m_videoThread) {
        m_videoThread->quit();          // 发送退出请求
        m_videoThread->wait(3000);      // 最多等待 3 秒
        delete m_videoThread;
        m_videoThread = nullptr;
        m_capturer = nullptr;           // 已被 deleteLater 销毁，置空防止悬空指针
    }
}
```

**为什么用 `BlockingQueuedConnection` 而不是直接调用？**  
`m_capturer` 已经 `moveToThread` 到视频线程，直接从主线程调用其方法是不安全的（线程违规）。`invokeMethod` 将调用投递到正确的线程，`BlockingQueuedConnection` 保证主线程等待其完成。

---

## 5. 音频线程：restartAudio

音频线程的创建和销毁被封装成 `restartAudio`，麦克风和系统音频用同一套逻辑：

```cpp
void SystemController::restartAudio(bool isMic, const QString& devId) {
    // 使用指针的指针，统一处理 mic 和 sys 两条路径
    WasapiCapturer** ptr = isMic ? &m_micCapturer : &m_sysCapturer;

    // 第一步：安全停止旧线程
    if (*ptr) {
        (*ptr)->stop();    // 设置 m_stopRequested = true
        (*ptr)->wait();    // 等待 run() 返回（最多若干秒）
        delete *ptr;
        *ptr = nullptr;
    }

    // 第二步：创建新的采集器并连接信号
    *ptr = new WasapiCapturer(this);
    (*ptr)->setDevice(devId, isMic ? WasapiCapturer::Microphone
                                   : WasapiCapturer::SystemLoopback);

    // 音量电平 → EventBus → UI 进度条
    connect(*ptr, &WasapiCapturer::volumeLevelChanged, this,
            [isMic](float l) {
                EventBus::instance()->fireAudioLevel(isMic, l);
            });

    // PCM 数据 → FFmpegRecorder 编码
    connect(*ptr, &WasapiCapturer::audioDataReady, m_recorder,
            [this, isMic](QByteArray d, int r, int c) {
                if (m_recorder)
                    m_recorder->pushAudioData(isMic ? 1 : 0, d, r, c);
            });

    (*ptr)->start();  // 启动线程
}
```

**`WasapiCapturer::stop()` + `wait()` 的重要性**：

```cpp
// WasapiCapturer.cpp
void WasapiCapturer::stop() {
    m_stopRequested = true;   // 原子写，run() 循环会在下一次检测时退出
}

void WasapiCapturer::run() {
    // ...
    while (!m_stopRequested) {   // 原子读
        // 采集 PCM 数据
    }
    // run() 返回 → 线程结束 → wait() 解除阻塞
}
```

如果不调用 `wait()`，直接 `delete *ptr`，线程可能还在 `run()` 里执行，销毁对象后访问成员变量 → 崩溃。

---

## 6. 生产者-消费者模式：视频帧的跨线程传递

这是整个系统中最核心的数据流设计：

```
【视频线程 - 生产者】          【主线程 - 中转】         【编码线程 - 消费者】
DxGiCapturer::captureLoop()
  │
  emit frameCaptured(img, ts)  ──QueuedConnection──>  SystemController::onVideoFrameCaptured()
                                                           │
                                                           │  m_recorder->pushVideoFrame(img, ts)
                                                           │       │
                                                           │       └─ QMutexLocker(&m_videoMutex)
                                                           │          m_videoQueue.enqueue({img, ts})
                                                                                    │
                                                               FFmpegRecorder::run() 循环
                                                                   │
                                                                   ├─ QMutexLocker(&m_videoMutex)
                                                                   ├─ m_videoQueue.dequeue()
                                                                   └─ processVideo(frameData)
                                                                        └─ 编码 → 写文件
```

### 6.1 pushVideoFrame：生产者端（加锁写）

```cpp
// FFmpegRecorder.cpp
void FFmpegRecorder::pushVideoFrame(const QImage& img, qint64 timestamp) {
    if (!m_isRecording || m_softStopRequested) return;  // 非录制状态丢弃

    QMutexLocker locker(&m_videoMutex);

    if (m_videoQueue.size() > 150) return;  // 队列满了也丢弃（防止内存爆炸）

    VideoFrameData data;
    // 统一转为 RGBA8888 格式（FFmpeg 编码需要）
    if (img.format() != QImage::Format_RGBA8888)
        data.image = img.convertToFormat(QImage::Format_RGBA8888);
    else
        data.image = img.copy();   // 深拷贝！防止视频线程修改时影响编码
    data.timestamp = timestamp;
    m_videoQueue.enqueue(data);
}
```

**队列上限 150 帧的意义**：  
30fps 下，150 帧 = 5 秒的缓冲。如果编码速度赶不上采集速度（如 CPU 过载），队列会积压。设置上限能防止内存无限增长，代价是丢弃过时帧（可接受）。

### 6.2 FFmpegRecorder::run()：消费者主循环

```cpp
void FFmpegRecorder::run() {
    // 初始化编码器（在编码线程中进行）
    if (!initVideoEncoder()) { emit errorOccurred("Video Encoder Init Failed"); return; }
    if (!initAudioEncoder()) { emit errorOccurred("Audio Encoder Init Failed"); return; }
    if (m_config.enableFileRecord)  initFileOutput();
    if (m_config.enableStreaming)   initStreamOutput();

    m_isRecording = true;   // 标志位：现在开始接受 push 进来的帧

    while (true) {
        QCoreApplication::processEvents();  // 处理投递到编码线程的事件（如停止请求）

        // ── 处理视频帧 ──
        VideoFrameData vFrameData;
        bool hasVideo = false;
        {
            QMutexLocker locker(&m_videoMutex);
            if (!m_videoQueue.isEmpty()) {
                vFrameData = m_videoQueue.dequeue();
                hasVideo = true;
            }
        }   // 锁在这里释放，持锁期间不做编码（防止长时间阻塞生产者）
        if (hasVideo) {
            processVideo(vFrameData);   // 编码视频帧
        }

        // ── 检测停止 → 进入淡出阶段 ──
        if (m_softStopRequested && !isFadingOut) {
            isFadingOut = true;
            // 计算淡出结束的采样点数
            fadeOutStartSample = m_totalAudioSamples;
            qint64 samplesNeeded = (qint64)(m_config.fadeOutDurationMs / 1000.0
                                             * m_config.audioSampleRate);
            finalAudioSampleEnd = fadeOutStartSample + samplesNeeded;
        }

        // ── 淡出完毕且队列清空 → 退出循环 ──
        if (isFadingOut
            && m_totalAudioSamples >= finalAudioSampleEnd
            && !hasVideo
            && m_videoQueue.isEmpty())
            break;

        // ── 处理音频帧（细节见第11课）──
        // ...

        if (!hasVideo && !m_softStopRequested)
            QThread::msleep(1);  // 空转时短暂休眠，避免 CPU 100%
    }

    flushEncoders();            // 冲刷编码器缓冲区
    av_write_trailer(m_fmtCtxFile);  // 写 MP4 文件尾（非常重要！）
    closeResources();
    m_isRecording = false;
}
```

**三个关键设计点**：

1. **`QCoreApplication::processEvents()`**：编码线程也需要处理 Qt 事件（因为 `FFmpegRecorder` 用了 `this->moveToThread(this)`），否则通过 invokeMethod 投递的调用无法执行。

2. **软停止（softStop）而非硬停止**：调用 `stopRecord()` 只设置 `m_softStopRequested = true`，循环会继续处理队列中剩余的帧，并完成音频淡出，最后才退出。这保证了录制文件完整性。

3. **`av_write_trailer`**：MP4 文件格式在文件末尾有索引表（moov box），必须调用 `av_write_trailer` 才能正确写入。如果程序崩溃没有写这个，MP4 文件无法播放（可以用 `ffmpeg -i broken.mp4 -c copy fixed.mp4` 修复）。

---

## 7. 摄像头资源池与帧池预热

### 7.1 问题背景

WinRT `MediaCapture` 初始化耗时约 500ms~2s（取决于摄像头驱动）。如果等用户点击"添加摄像头"才开始初始化，用户体验极差——点击后要等好几秒才出现画面。

### 7.2 解决方案：帧池预热

```cpp
// 在 D3D 设备初始化完成后立即触发
void SystemController::onCapturerInitFinished(bool success) {
    if (success) {
        onRefreshDevices();      // 刷新设备列表
        restartAudio(false, ""); // 启动系统音频采集
        prewarmAllCameras();     // 预热所有摄像头 ← 核心
    }
}

void SystemController::prewarmAllCameras() {
    if (!m_capturer) return;

    // 在视频线程中执行（因为需要 D3D 设备）
    QMetaObject::invokeMethod(m_capturer, [this]() {
        const auto cameras = QMediaDevices::videoInputs();  // 枚举所有摄像头
        ID3D11Device* rawDevice = m_capturer->getD3DDevice();
        if (!rawDevice) return;

        QMutexLocker locker(&m_poolMutex);
        for (const auto& info : cameras) {
            QString deviceId = QString::fromUtf8(info.id());
            if (m_cameraPool.contains(deviceId)) continue;  // 已预热跳过

            WinRTCamera* cam = new WinRTCamera();
            if (cam->init(rawDevice) && cam->start(deviceId)) {
                m_cameraPool.insert(deviceId, cam);  // 放入资源池
                LOG_INFO("Camera prewarmed: " + deviceId);
            } else {
                delete cam;
            }
        }
    });
}
```

**预热时间线**：

```
程序启动
  │
  ├─ 主线程：SystemController::initialize() → startVideoThread()
  │
  ├─ 视频线程启动：init() → D3D设备创建完成
  │       │ emit initFinished(true)
  │       ▼
  ├─ 主线程：onCapturerInitFinished() → prewarmAllCameras()
  │       │ invokeMethod(视频线程)
  │       ▼
  └─ 视频线程：WinRTCamera::init() + start()（后台静默预热，不阻塞UI）

用户点击"添加摄像头"（约2~5秒后）
  │
  ▼
onAddCamera() → 查 m_cameraPool → 直接找到预热好的实例 → 立即显示画面 ✅
```

### 7.3 onAddCamera：资源池的使用

```cpp
void SystemController::onAddCamera(QVariant deviceVariant) {
    QCameraDevice info = deviceVariant.value<QCameraDevice>();
    QString deviceId = QString::fromUtf8(info.id());

    // 防止同一摄像头重复添加
    {
        QMutexLocker locker(&m_poolMutex);
        for (auto ctx : m_activeLayers) {
            if (ctx->deviceId == deviceId) return;  // 已添加，直接返回
        }
    }

    // 查找预热池
    WinRTCamera* targetCam = nullptr;
    {
        QMutexLocker locker(&m_poolMutex);
        if (m_cameraPool.contains(deviceId))
            targetCam = m_cameraPool.value(deviceId);
    }

    int id = m_nextItemId++;
    QRect defaultRect(50, 50, 320, 240);

    // 建立图层的 Lambda（两条路径共用）
    auto setupLayer = [=](WinRTCamera* cam) {
        m_capturer->addCameraOverlay(id, cam, defaultRect);  // 通知渲染器添加图层

        CameraContext* ctx = new CameraContext;
        ctx->itemId   = id;
        ctx->deviceId = deviceId;
        ctx->camera   = cam;
        {
            QMutexLocker l(&m_poolMutex);
            m_activeLayers.append(ctx);     // 记录活跃图层
        }
        EventBus::instance()->fireCameraAdded(id, defaultRect);  // 通知 UI 创建控件
    };

    if (targetCam) {
        // ✅ 快速路径：预热池中有 → 直接使用
        setupLayer(targetCam);
    } else {
        // ⚡ 慢速路径：预热失败（或池未准备好）→ 临时创建
        QMetaObject::invokeMethod(m_capturer, [=]() {
            ID3D11Device* device = m_capturer->getD3DDevice();
            WinRTCamera* cam = new WinRTCamera();
            if (cam->init(device) && cam->start(deviceId)) {
                { QMutexLocker l(&m_poolMutex); m_cameraPool.insert(deviceId, cam); }
                // 创建完成后回到主线程 setupLayer
                QMetaObject::invokeMethod(this, [=]() { setupLayer(cam); });
            } else {
                delete cam;
                EventBus::instance()->fireError("Failed to open camera");
            }
        });
    }
}
```

注意 `invokeMethod` 的嵌套：
- 外层：主线程 → 视频线程（在视频线程中创建摄像头，因为需要 D3D 设备）
- 内层：视频线程 → 主线程（创建完成后回到主线程更新 UI 状态）

---

## 8. 析构：有序清理的艺术

析构函数是最容易引发崩溃的地方，顺序错误会导致线程访问已销毁的对象。

```cpp
SystemController::~SystemController() {
    LOG_INFO("SystemController Destroying...");

    // ① 先停编码线程（消费者）
    if (m_recorder) {
        m_recorder->stopRecord();   // 软停止：设置标志位
        m_recorder->wait();         // 等待编码线程 run() 返回
        delete m_recorder;          // 安全销毁
        m_recorder = nullptr;
    }

    // ② 再停视频采集线程（生产者之一）
    stopVideoThread();

    // ③ 清理摄像头资源池（依赖 D3D 设备，必须在视频线程停止前？）
    cleanupCameraPool();

    // ④ 最后停音频线程（另一路生产者）
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
    LOG_INFO("SystemController Destroyed.");
}
```

**为什么编码线程先停？**

```
如果先停视频线程：
  视频线程停止 → 不再 push 帧 → 编码线程队列为空
  编码线程 run() 检测到 m_softStopRequested=false，继续空转 → 永远不退出！

正确顺序：
  编码线程 stopRecord() → m_softStopRequested=true → 编码线程自然退出
  → wait() 确认退出 → 再停视频线程（安全）
```

---

## 9. 状态机视角：SystemController 的状态

虽然代码中没有显式的状态枚举，但 SystemController 隐含了以下状态机：

```
         initialize()
              │
              ▼
         ┌─────────┐
         │  就绪   │ ← D3D未初始化，采集器未启动
         └────┬────┘
              │ onCapturerInitFinished(true)
              ▼
         ┌─────────┐
         │  待机   │ ← 采集器运行（仅预览），编码器未启动
         └────┬────┘
              │ cmd_toggleRecording(true)
              ▼
         ┌─────────┐
    ┌──→ │  录制中  │ ← 采集器+编码器双线程运行
    │    └────┬────┘
    │         │ cmd_toggleRecording(false)
    │         ▼
    │    ┌─────────┐
    │    │  停止中  │ ← 音频淡出，队列清空
    │    └────┬────┘
    └────────┘  编码完成，回到待机状态
```

**状态的体现**：
- `m_isRecording`（在 FFmpegRecorder 中）：是否在录制
- `m_softStopRequested`（在 FFmpegRecorder 中）：是否正在优雅停止
- `m_durationTimer->isActive()`：是否在计时（间接反映录制状态）

---

## 10. 本课作业

1. **线程归属验证**：在 `SystemController::onVideoFrameCaptured` 中加一行 `qDebug() << "Main thread?" << (QThread::currentThread() == qApp->thread());`，运行程序观察输出，验证这个槽确实在主线程执行（项目中已有类似代码）。

**答案：**

这个验证在当前项目里其实已经加上了，位置在 `SystemController.cpp:388` 附近：

```cpp
void SystemController::onVideoFrameCaptured(const QImage &img, qint64 ts)
{
    m_recorder->pushVideoFrame(img, ts);
    qDebug() << QThread::currentThreadId();
    qDebug() << "Main thread?" << (QThread::currentThread() == qApp->thread());
}
```

之所以它会输出 `true`，核心原因是 `SystemController::startVideoThread()` 里这条连接使用了 **`Qt::QueuedConnection`**：

```cpp
connect(m_capturer,
        &DxGiCapturer::frameCaptured,
        this,
        &SystemController::onVideoFrameCaptured,
        Qt::QueuedConnection);
```

这里的执行链是：

```text
DxGiCapturer（视频线程）
  └─ emit frameCaptured(img, ts)
       └─ Qt::QueuedConnection 跨线程投递
            └─ SystemController::onVideoFrameCaptured(...)（主线程执行）
```

所以运行时预期现象是：
- `QThread::currentThread() == qApp->thread()` 输出 `true`
- 说明这个槽并不是在视频线程直接执行，而是被投递回了主线程
- 这也验证了教程里“视频线程采集 → 主线程中转 → 编码线程消费”的设计是成立的

2. **资源池分析**：阅读 `cleanupCameraPool()`，思考：为什么 `m_activeLayers` 中的 `CameraContext` 对象需要 `delete`，而 `m_cameraPool` 中的 `WinRTCamera` 对象也需要 `stop()` 后才能 `delete`？如果直接 `delete` 不调用 `stop()` 会怎样？

**答案：**

先看 `cleanupCameraPool()`：

```cpp
void SystemController::cleanupCameraPool()
{
    QMutexLocker locker(&m_poolMutex);
    for (auto cam : m_cameraPool) {
        cam->stop();
        delete cam;
    }
    m_cameraPool.clear();
    for (auto ctx : m_activeLayers)
        delete ctx;
    m_activeLayers.clear();
}
```

这里两类对象性质不一样：

### （1）为什么 `CameraContext` 只需要 `delete`

`CameraContext` 本质上只是一个**普通的数据记录结构**，定义在 `SystemController.h:20` 左右：

```cpp
struct CameraContext {
    int itemId;
    QString deviceId;
    WinRTCamera* camera = nullptr;
};
```

它不负责开线程、不负责采集、不持有事件循环，只是把：
- 图层 ID
- 摄像头设备 ID
- 对应的 `WinRTCamera*`

绑定在一起，方便 `SystemController` 追踪“哪个图层对应哪个物理摄像头”。

因此它没有“停止”动作，只要确认不用了，直接 `delete` 即可，否则会造成堆内存泄漏。

### （2）为什么 `WinRTCamera` 必须先 `stop()` 再 `delete`

`WinRTCamera` 不只是一个数据壳，它背后是真正运行中的摄像头采集对象，可能还持有：
- WinRT `MediaCapture`
- 摄像头驱动资源
- 帧回调/帧池
- GPU 纹理或相关句柄

这类对象一般都存在“后台仍在工作”的可能。`stop()` 的意义是：
- 先告诉摄像头采集逻辑停止出帧
- 解除对底层设备/回调/帧池的占用
- 让对象进入可安全销毁的静止状态

然后再 `delete`，才是安全的资源回收顺序。

### （3）如果直接 `delete` 不调用 `stop()`，可能发生什么

可能会出现几类典型问题：

1. **后台回调访问已释放对象**  
   摄像头线程/回调还在跑，但 C++ 对象已经被释放，下一次访问成员变量就可能直接崩溃。

2. **底层设备句柄没有正常释放**  
   摄像头可能仍被系统占用，导致下次重新打开设备失败，出现“摄像头被占用”或无法重连。

3. **资源析构顺序错乱**  
   比如纹理、帧池、WinRT 对象还没停，外层对象先没了，容易造成悬空指针或未定义行为。

4. **偶发性 bug 最难查**  
   有时候不是立刻崩，而是在退出程序、切换场景、重新添加摄像头时随机崩溃，这类问题通常就是“没停干净就删对象”。

**总结：**
- `CameraContext` 是“记录关系”的轻量对象，所以直接 `delete`
- `WinRTCamera` 是“活着的资源对象”，所以必须先 `stop()` 再 `delete`
- 这本质上是“先停业务活动，再释放内存”的工程原则

3. **模拟队列溢出**：在 `pushVideoFrame` 中，当前队列上限是 150 帧。如果把这个限制去掉（注释掉 `if (m_videoQueue.size() > 150) return;`），在低端电脑上长时间录制会发生什么？请分析可能的后果。

**答案：**

当前代码在 `FFmpegRecorder.cpp:65` 附近：

```cpp
void FFmpegRecorder::pushVideoFrame(const QImage &img, qint64 timestamp) {
    if (!m_isRecording || m_softStopRequested) return;
    QMutexLocker locker(&m_videoMutex);
    if (m_videoQueue.size() > 150) return;
    ...
    m_videoQueue.enqueue(data);
}
```

这个 150 帧上限，本质上是一个**背压保护**。如果把它去掉，在低端电脑上最可能发生的是：

### （1）内存持续上涨

视频线程采集帧的速度通常接近固定帧率，比如 30fps；但编码线程 `processVideo()` 的速度受 CPU、磁盘、FFmpeg 编码压力影响。

如果：
- 采集速度 > 编码速度

那么 `m_videoQueue` 就会越来越长。每一帧都是一个 `QImage` 深拷贝，分辨率如果是 1920×1080，单帧内存大约就是几 MB 量级。长时间积压后，内存会迅速膨胀。

结果可能是：
- 进程占用内存越来越高
- 系统开始频繁换页
- 整机变卡
- 最后程序被系统杀掉，或者分配失败崩溃

### （2）录制延迟越来越大

就算程序没立刻崩，编码线程处理的是“越来越旧的帧”。

表现为：
- 用户已经停止操作，但编码线程还在慢慢消化旧数据
- 停止录制后要等很久文件才真正写完
- 预览看起来是当前画面，但写入文件的是几秒甚至几十秒之前的画面

这会让系统出现严重的“采集实时性丧失”。

### （3）停止录制会变得很慢

当前录制停止是“软停止”：
- `stopRecord()` 只设置 `m_softStopRequested = true`
- 编码线程还要把队列里剩余数据处理完，再淡出音频，再写 trailer

如果视频队列无限大，那么停止时可能已经积压了大量帧。结果就是：
- 点了“停止”以后程序长时间无响应
- 需要等很久才能真正结束录制
- 用户体验会非常差

### （4）音视频同步更容易出问题

视频大量积压而音频继续按当前节奏编码，可能带来：
- 视频时间戳对应的是过去的数据
- 音频和视频处理节奏不再一致
- 最终文件可能出现音画不同步加重的问题

### （5）锁竞争和 CPU 压力会进一步加重

队列变大后：
- `enqueue/dequeue` 操作更频繁
- 内存分配和拷贝更多
- CPU 更多时间消耗在搬运旧帧，而不是处理当前帧

最终可能形成恶性循环：机器越卡 → 编码越慢 → 队列积压越快。

**为什么丢帧反而是正确策略？**

因为录屏系统的优先目标通常是：
- 稳定运行
- 不爆内存
- 保持尽量接近实时

所以当编码来不及时，**丢掉旧帧比无限堆积更合理**。这也是 `150` 帧上限存在的意义：
- 用有限内存换系统稳定
- 允许在极端情况下牺牲少量画面完整性，避免整段录制彻底失败

**总结：**
去掉队列上限后，低端电脑长时间录制最可能出现的后果是：**内存暴涨、停止变慢、系统卡顿、音画延迟增大，最终甚至崩溃**。

4. **扩展实践——暂停功能**：根据本课对状态机的分析，设计"暂停录制"功能的实现方案：
   - 需要在 `EventBus` 中增加什么信号？
   - `FFmpegRecorder::run()` 的主循环需要做什么修改来实现暂停？
   - 暂停期间音频数据如何处理（继续推入还是丢弃）？
   - 恢复录制时，PTS 时间戳该如何处理（直接延续还是重新计算）？

**答案：**

我会把“暂停”设计成**录制状态机中的一个显式状态**，而不是简单加个布尔变量随便跳。

---

### （1）EventBus 需要增加什么信号？

我建议加两类：

#### ① 命令信号
用于 UI 通知控制层执行暂停/恢复：

```cpp
void sendCommandTogglePause(bool paused) { emit cmd_togglePause(paused); }

signals:
    void cmd_togglePause(bool paused);
```

这属于 **cmd 类**，方向是 **UI → Controller**。

#### ② 状态信号
用于控制层把“当前是否暂停”同步给 UI：

```cpp
void firePauseStateChanged(bool paused) { emit state_pauseChanged(paused); }

signals:
    void state_pauseChanged(bool paused);
```

这属于 **state 类**，方向是 **Controller → UI**。

这样 UI 层按钮点击只负责发命令，真正状态变化还是由控制层统一广播，和现有架构风格一致。

---

### （2）`FFmpegRecorder::run()` 主循环需要怎么改？

核心思路是：**暂停时线程不退出，但停止消费音视频队列，也不再推进编码时间线。**

可以在 `FFmpegRecorder` 中新增：

```cpp
std::atomic<bool> m_isPaused{false};
qint64 m_totalPausedMs = 0;
qint64 m_pauseStartMs = 0;
```

再提供槽或方法：

```cpp
void setPaused(bool paused);
```

在 `run()` 主循环中，加入暂停判断：

```cpp
while (true) {
    QCoreApplication::processEvents();

    if (m_isPaused) {
        QThread::msleep(5);
        continue;
    }

    // 原有的视频/音频消费、编码逻辑
}
```

但更严谨一点，还要结合“生产端是否继续入队”一起设计，否则暂停期间队列会越积越大。

所以单靠 `run()` 里 `continue` 不够，必须配套让 `pushVideoFrame()` / `pushAudioData()` 也识别暂停状态。

---

### （3）暂停期间音频数据应该继续推入还是丢弃？

我建议：**直接丢弃，不继续推入。**

也就是暂停期间：
- 视频帧不入队
- 音频 PCM 不入队
- 编码线程也不编码

原因有三个：

1. **暂停的语义就是“这段内容不录进去”**  
   如果暂停时还继续缓存音频，那恢复后会把暂停期间声音补进去，和用户预期相反。

2. **防止队列膨胀**  
   暂停可能持续几分钟，如果继续推入音视频数据，内存一定爆。

3. **恢复逻辑更简单**  
   丢弃暂停期间数据后，恢复时只需要从新的采样点继续，不需要清理历史缓存。

所以我会让：

```cpp
void FFmpegRecorder::pushVideoFrame(...) {
    if (!m_isRecording || m_softStopRequested || m_isPaused) return;
    ...
}

void FFmpegRecorder::pushAudioData(...) {
    if (!m_isRecording || m_softStopRequested || m_isPaused) return;
    ...
}
```

同时 `SystemController` 也可以通过 `QMetaObject::invokeMethod(m_capturer, "stopRecording", ...)` 或额外的 pause 标记，让采集器在暂停期间不再发出录制帧，只保留预览帧。

也就是说：
- **预览继续**（用户还能看到画面）
- **录制数据停止入队**

这是最符合录屏软件体验的。

---

### （4）恢复录制时 PTS 该如何处理？

我建议：**不要直接沿用墙上时间，而是要“扣除暂停时长”后重新计算连续时间戳。**

也就是说，恢复后生成的 PTS 要表现得像“暂停这段时间从未存在过”。

#### 为什么不能直接延续原始时间？

如果直接使用真实时间：
- 暂停前最后一帧是 10s
- 暂停了 20s
- 恢复后下一帧时间戳变成 30s

那导出文件里就会出现一大段空洞：
- 画面像被冻结
- 音频可能断裂
- 播放器看到时间戳跳跃，可能产生卡顿或异常停顿

这不是“暂停录制”，而更像“录了一个长静止段”。

#### 正确做法
维护一个“累计暂停时长”变量：

```cpp
adjustedPts = originalPts - totalPausedDuration;
```

例如：
- 录到 10s 时暂停
- 暂停 20s
- 恢复后真实时间到 30s
- 但写入编码器的时间戳应该是 10s 继续往后走，而不是 30s

这样输出文件时间轴才连续。

对当前项目来说，可以在 `SystemController` 或 `FFmpegRecorder` 中统一做这个折算：
- 如果视频 PTS 主要来自 `DxGiCapturer::frameCaptured(..., timestamp)`，那就需要在送进 `pushVideoFrame()` 前做暂停补偿
- 或者干脆在 `FFmpegRecorder` 内部维护自己的连续录制时钟，按“已实际录制时长”生成 PTS

我更推荐第二种：**编码器内部维护连续录制时间轴**，不要直接相信外部墙钟时间，这样最稳定。

---

### 一个完整的实现思路

```text
UI 点击“暂停”
  └─ EventBus::cmd_togglePause(true)
       └─ SystemController::onTogglePause(true)
            ├─ 通知 FFmpegRecorder 进入 paused
            ├─ 通知 DxGiCapturer 停止发录制帧（但继续预览）
            └─ EventBus::state_pauseChanged(true)

UI 点击“恢复”
  └─ EventBus::cmd_togglePause(false)
       └─ SystemController::onTogglePause(false)
            ├─ 记录本次暂停持续时间，累加到 totalPausedDuration
            ├─ 通知 FFmpegRecorder 恢复
            ├─ 通知 DxGiCapturer 恢复发录制帧
            └─ EventBus::state_pauseChanged(false)
```

**最终结论：**
- EventBus 增加 `cmd_togglePause(bool)` 和 `state_pauseChanged(bool)`
- `FFmpegRecorder::run()` 增加暂停状态判断，但线程不退出
- 暂停期间音视频数据都应该丢弃，不继续入队
- 恢复时 PTS 不能直接沿用真实时间，而要扣除暂停时长，保证输出文件时间轴连续

---

*上一课：[第3课：项目整体架构与 EventBus](./第3课：项目整体架构与EventBus.md)*  
*下一课：[第5课：DXGI 屏幕采集](./第5课：DXGI屏幕采集.md)*
