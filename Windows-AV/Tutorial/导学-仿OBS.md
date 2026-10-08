# 导学：仿OBS

> 简称确认：`仿OBS`  
> 技术栈：Qt 6.5 / FFmpeg 7 / Direct3D11 / DXGI Desktop Duplication / Windows Graphics Capture / WASAPI / WinRT MediaCapture / C++20 / MSVC  
> 求职方向：音视频开发（Windows 客户端）

---

## 1. 前置知识（面试高频标注）

| 知识点 | 为何需要 | 在本项目中的位置 | 高频度 |
|---|---|---|---|
| DXGI Desktop Duplication API | 抓取显示器后缓冲帧的底层 Windows 接口 | `DxGiCapturer.cpp` `initDuplication()` | 高 |
| Windows Graphics Capture (WGC) | 捕获单窗口（含 DWM 混合帧）的 WinRT API | `WgcCapturer.cpp/h` | 高 |
| Direct3D11 纹理 / SRV / RTV / BlendState | GPU 合成 overlay 的核心资源 | `DxGiCapturer.h` ComPtr 成员群 | 高 |
| WASAPI（音频会话 API） | Windows 系统 Loopback + 麦克风采集 | `WasapiCapturer.cpp/h` | 高 |
| FFmpeg 编码管线（avcodec / avformat） | H264 编码 + MP4/FLV 封装 + RTMP 推流 | `FFmpegRecorder.cpp` | 高 |
| SwrContext + AVAudioFifo | 音频重采样 + 多源样本级混音缓冲 | `FFmpegRecorder.cpp` `resampleToFifo()` | 高 |
| PTS / DTS 时间戳 + av_rescale_q | 音视频同步与 RTMP 包序列正确性 | `FFmpegRecorder.cpp` `writePacket()` | 高 |
| WinRT MediaCapture + 格式协商 | UWP 摄像头访问、评分选格式、帧回调 | `WinRTCamera.cpp` `start()` | 中 |
| Qt QThread + QMetaObject::invokeMethod | 多线程间安全跨线程调用 Slot | `SystemController.cpp` 多处 | 高 |
| COM / WRL::ComPtr | Windows COM 资源 RAII 管理 | 全局多处 | 中 |
| RTMP 协议 + FLV 封装 | 直播推流格式要求 | `FFmpegRecorder.cpp` `initStreamOutput()` | 中 |
| RoInitialize / COINIT_MULTITHREADED | WinRT 运行时多线程初始化 | `SystemController.cpp` `startVideoThread()` 内 lambda | 中 |

---

## 2. 重点亮点与学习顺序（先看这个）

### 亮点 1：双后端采集切换（DXGI + WGC）
**为什么重要**：能回答"DXGI 为什么抓不到部分窗口/安全桌面"，是本项目最高频考点。  
**先看文件**：`DxGiCapturer.h`（`CaptureSourceType`、`CaptureTarget`）→ `DxGiCapturer.cpp` `setCaptureTarget()` → `WgcCapturer.cpp` `startCapture(hwnd)`  
**学习顺序**：1

---

### 亮点 2：Direct3D11 GPU 合成叠加管线
**为什么重要**：GPU 合成 overlay 是性能关键，展示你对渲染资源（SRV/RTV/Shader/BlendState）的掌握。  
**先看文件**：`DxGiCapturer.h` 成员（`m_compositionTexture`、`m_compositionRTV`、`m_blendState`、Shader ComPtr）→ `DxGiCapturer.cpp` `renderTexture()` → `updateTextureFromImage()`  
**学习顺序**：2

---

### 亮点 3：多源音频混音架构
**为什么重要**：展示对 `AVAudioFifo` + `SwrContext` + Float32 planar 混音的掌握，是音视频后台必考题。  
**先看文件**：`FFmpegRecorder.h` `SourceContext` 结构体 → `FFmpegRecorder.cpp` `resampleToFifo()` → `run()` 音频编码循环中 `mixBufferL[i] += tmpL[i]` 段  
**学习顺序**：3

---

### 亮点 4：FFmpeg 双输出同步（本地 MP4 + RTMP 推流）
**为什么重要**：一次编码双路输出是大厂面试高频考点，涉及 PTS 重置、FLV 封装、TCP 参数调优。  
**先看文件**：`FFmpegRecorder.h`（`m_fmtCtxFile` + `m_fmtCtxStream` + `m_lastStreamVideoPts`）→ `FFmpegRecorder.cpp` `initStreamOutput()` → `writePacket()`  
**学习顺序**：4

---

### 亮点 5：WinRT 摄像头预热池 + 格式协商评分
**为什么重要**：展示延迟优化（预热）和格式选择（规避 MJPG 解码开销）的工程判断力。  
**先看文件**：`SystemController.cpp` `prewarmAllCameras()` → `WinRTCamera.cpp` `start()` 格式评分段 → `onFrameArrived()` SoftwareBitmap::Convert  
**学习顺序**：5

---

### 亮点 6：EventBus + 三线程解耦架构
**为什么重要**：能讲清楚 UI / 采集 / 录制三线程如何解耦，是系统设计题的高质量样板答案。  
**先看文件**：`EventBus.h` → `main.cpp` → `SystemController.cpp` `initialize()` + `startVideoThread()`  
**学习顺序**：6

---

## 3. 必备知识点

- [ ] DXGI Desktop Duplication 与 WGC 的区别：适用场景 / DWM 保护限制 / 帧率表现
- [ ] D3D11 纹理类型（DEFAULT / STAGING / DYNAMIC）及 GPU→CPU 数据流（Map/Unmap）
- [ ] WASAPI Loopback 与麦克风采集的初始化差异（eRender/eCapture + AUDCLNT_STREAMFLAGS_LOOPBACK）
- [ ] FFmpeg 编码器初始化流程：avcodec_open2 → avformat_write_header → 帧→包循环 → flush → write_trailer
- [ ] SwrContext 重采样 + AVAudioFifo 的配合：push 原始样本 → av_audio_fifo_size → pull frame_size 样本
- [ ] PTS/DTS 计算与时间基转换：`av_rescale_q(pts, src_tb, dst_tb)`
- [ ] RTMP FLV 封装要求：`codec_tag = 0`、强制 flv 格式名、`avio_open2` 传 options
- [ ] Qt 跨线程调用的三种方式：`QueuedConnection` / `BlockingQueuedConnection` / `QMetaObject::invokeMethod`
- [ ] WinRT C++/WinRT `.get()` 同步等待协程与 `RoInitialize(RO_INIT_MULTITHREADED)` 要求
- [ ] 音频淡入淡出原理：按样本偏移线性插值增益系数，在编码线程内对混音缓冲区逐样本应用

---

## 4. 推荐阅读（结合仓库）

| 主题 | 建议阅读位置 | 预计时间 | 读完能回答什么 |
|---|---|---|---|
| 双后端采集切换逻辑 | `DxGiCapturer.cpp` `setCaptureTarget()` + `startCapture()` | 30 min | 为什么显示器用 DXGI、窗口用 WGC？切换时如何避免崩溃？ |
| GPU overlay 合成全流程 | `DxGiCapturer.h` 成员列表 + `DxGiCapturer.cpp` `renderTexture()` | 40 min | 如何把多个图层（图片/摄像头）用 D3D11 shader 合成为一帧？ |
| 音频混音管线 | `FFmpegRecorder.cpp` `resampleToFifo()` + `run()` 音频段 | 40 min | 麦克风和系统音如何对齐时序并混成一条 AAC 流？ |
| RTMP 推流参数配置 | `FFmpegRecorder.cpp` `initStreamOutput()` | 20 min | 为什么设置 `tcp_nodelay` / `rtmp_buffer` / `rw_timeout`？ |
| 摄像头格式协商与预热 | `WinRTCamera.cpp` `start()` 格式评分段 + `SystemController.cpp` `prewarmAllCameras()` | 30 min | BGRA8 优先的原因？预热池怎么降低首次添加延迟？ |
| 线程架构与 EventBus | `EventBus.h` + `SystemController.cpp` `startVideoThread()` + `main.cpp` | 20 min | 三个线程各自职责？跨线程调用为什么用 invokeMethod 而非直接调用？ |
| 音频淡入淡出 | `FFmpegRecorder.cpp` `applyFadeEffect()` | 15 min | Fade 的实现原理？为什么在编码循环内处理而非 capture 侧？ |
| WGC 窗口捕获 | `WgcCapturer.cpp/h` | 25 min | WGC 如何绕过 DXGI 不能捕获 DWM 混合帧的限制？ |

---

## 5. 自学提醒

本导学文档给出学习路径与面试题目，**不提供逐行代码讲解**。若阅读某文件时遇到看不懂的 API（如 `IDXGIOutputDuplication::AcquireNextFrame`、`av_audio_fifo_alloc`、`MediaFrameReader.FrameArrived`），**请直接向 AI 追问该 API 的原理和用法**，不要卡在此处影响整体进度。本 skill 的价值在于给你"先看什么、能问什么"，细节理解靠你主动追问。

---

## 6. 项目技术定位

**交叉（系统软件 + 音视频中间件）**：项目深度依赖 Windows 图形栈（D3D11 / DXGI / WGC / WinRT）和操作系统音频栈（WASAPI / COM），同时集成跨平台音视频编码库（FFmpeg），属于典型的 Windows 音视频客户端系统软件方向，非 Web / 纯后端。

---

## 7. 核心原理解析

### 7.1 为什么 DXGI Desktop Duplication 捕获不了部分窗口

**问题**：录制游戏全屏或受保护的 DRM 内容时，DXGI 抓到的帧是黑屏。  
**机制**：DXGI 输出复制（`IDXGIOutputDuplication`）捕获的是整块显示器的 DDI 层面合成输出，DWM 对特定窗口（带 `WDA_EXCLUDEFROMCAPTURE` 标志或全屏独占模式的游戏）会拒绝输出到 Duplication 接口。WGC 通过 `GraphicsCaptureItem` 让系统以受信任身份捕获，绕开此限制。  
**落点**：`DxGiCapturer.h` 中 `CaptureSourceType { Monitor, Window }` 枚举控制后端选择；`setCaptureTarget()` 在 type = Window 时切换到 `m_wgcCapturer->startCapture(hwnd)`，Monitor 模式继续使用 `initDuplication(monitorIndex, ...)`。

---

### 7.2 多图层 GPU 合成的帧生命周期

**问题**：如何高效地将桌面帧 + N 张图片 + M 路摄像头合成为一张可编码的 QImage？  
**机制**：每个 overlay 在 GPU 端维护独立 `ID3D11Texture2D` + `ID3D11ShaderResourceView`；每一帧先把桌面纹理渲染到 `m_compositionTexture`（绑定 `m_compositionRTV` 作为 RenderTarget），然后按 `m_renderOrder` 依次调用 `renderTexture()` 用 `m_blendState`（alpha blend）叠加各 overlay；最终 CopyResource 到 `m_stagingTexture`，CPU Map 读出 BGRA 字节流构造 QImage 投入编码队列。  
**落点**：`DxGiCapturer.h` 中的 `m_compositionRTV`、`m_blendState`、`m_stagingTexture`；`DxGiCapturer.cpp` `renderTexture()` 函数负责单层渲染。

---

### 7.3 多源音频如何对齐并混成一路 AAC

**问题**：麦克风（48kHz Float32）和系统音（44.1kHz Float32）采样率不同、到达时间不同，如何混成一路标准 AAC？  
**机制**：每个音频源（id=0 系统音，id=1 麦克风）有独立 `SwrContext`（重采样到目标 44100 Hz Float32 planar stereo）和 `AVAudioFifo`（样本级弹性缓冲）；编码线程驱动，每次从各 fifo 读 `frame_size` 个样本（若不足则补零），在 `mixBufferL/R` 上逐样本加法，`std::clamp` 防削波，再送 `avcodec_send_frame`；时序由视频 PTS `av_rescale_q` 换算到目标采样数来驱动。  
**落点**：`FFmpegRecorder.cpp` `resampleToFifo()` + `run()` 内 `while (m_totalAudioSamples < targetSamples)` 循环。

---

### 7.4 双输出时 RTMP 流为什么需要独立 PTS 序列

**问题**：文件录制和 RTMP 推流共用同一个编码器，为什么要分别维护时间戳？  
**机制**：RTMP 是连续时间流，接收端（流媒体服务器）会检测 DTS 单调递增性；而文件输出允许 PTS 相对编码器时间基，两者时间基不同（文件通常 1/90000，RTMP FLV 通常 1/1000）；`writePacket()` 针对 stream 输出做 `av_rescale_q` 并使用 `m_lastStreamVideoPts/Dts` 跟踪保证单调性；若重新推流则从 0 开始。  
**落点**：`FFmpegRecorder.h` `m_lastStreamVideoPts/Dts/AudioPts/Dts` 4 个成员；`FFmpegRecorder.cpp` `writePacket()` 分支处理。

---

### 7.5 WinRT 摄像头格式协商与颜色空间统一

**问题**：不同摄像头支持不同格式（NV12 / YUY2 / MJPG / BGRA8），如何保证 GPU 端格式统一？  
**机制**：`start()` 遍历所有 `VideoEncodingProperties` 并评分（BGRA8 +3000 > NV12 +2000 > YUY2 +1000 > MJPG +500），选分最高格式 `SetMediaStreamPropertiesAsync`；在 `onFrameArrived()` 中强制 `SoftwareBitmap::Convert(BGRA8, Premultiplied)` 统一颜色空间，随后 CPU 端 BGRA 数据上传到 `DXGI_FORMAT_B8G8R8A8_UNORM` 纹理，无需额外 Shader 转换。  
**落点**：`WinRTCamera.cpp` 格式评分 + `onFrameArrived()` 内 `SoftwareBitmap::Convert` + `DXGI_FORMAT_B8G8R8A8_UNORM` 纹理描述。

---

### 7.6 采集线程与录制线程的背压机制

**问题**：如果编码跟不上采集速度，如何防止内存无限增长？  
**机制**：`FFmpegRecorder::pushVideoFrame()` 在锁保护下检查 `m_videoQueue.size() > 150`，超过则直接 return 丢弃当前帧；`m_uiInFlight` 原子计数控制 preview 帧的 UI 处理节奏（UI 消费完才允许下一帧进入 Scene）；音频队列无硬上限，但 AVAudioFifo 内存有界且编码线程持续消费。  
**落点**：`FFmpegRecorder.cpp` `pushVideoFrame()` 中 `if (m_videoQueue.size() > 150) return`；`DxGiCapturer.h` `m_uiInFlight` atomic 成员。

---

## 8. 关键设计决策

| 决策点 | 备选方案 | 取舍理由 | 风险 | 验证措施 |
|---|---|---|---|---|
| 视频采集独立 QThread | 在主线程轮询 | DXGI `AcquireNextFrame` 有阻塞等待，放主线程会冻结 UI | 线程销毁时需 BlockingQueuedConnection 同步 stopCapture | `SystemController::stopVideoThread()` 已处理 |
| 摄像头用 WinRT MediaCapture 而非 DirectShow | DirectShow / Media Foundation | WinRT 提供统一格式协商 API，支持 SharedReadOnly 多 Consumer | 依赖 WinRT 运行时，需 RoInitialize(MULTITHREADED) | `startVideoThread` lambda 内优先 RoInitialize |
| 音频混音在录制线程而非采集线程 | 在 WasapiCapturer 侧混音 | 录制线程有完整时间基，混音与视频 PTS 对齐更准确 | 采集速率高于编码时 FIFO 可能积压 | AVAudioFifo 弹性缓冲兜底；无视频帧时 msleep(1) |
| EventBus 单例信号总线 | 直接 connect 各对象 | 解耦 View / Controller，便于后期替换后端或添加测试 | 全局单例不支持多实例场景 | 当前为单进程单窗口，无影响 |
| RTMP 强制 TCP + tcp_nodelay | RTMP over UDP / QUIC | TCP 更稳定，tcp_nodelay 禁用 Nagle 减少首包延迟 | 弱网下 TCP 重传增加端到端延迟 | `rw_timeout=5000000 µs` 防止卡死 |
| 摄像头预热池 | 按需 init | MediaCapture.InitializeAsync 耗时约 1~2s，预热后切换即用 | 多摄像头时 D3D 资源占用增加 | D3D 设备 init 完成回调（`onCapturerInitFinished`）后启动预热 |

---

## 9. 量化与验证（含待测，建议）

以下为建议测量思路，当前项目暂无实测基准数据，面试时诚实说明"已设计测量方案，尚未完成基准测试"。

**① 采集帧率稳定性**
- 环境：1920×1080 60fps 目标，NVIDIA GPU，Win11
- 指标：`frameCaptured` 信号相邻时间戳差的标准差 < 2ms
- 工具：在 `DxGiCapturer.cpp` `frameCaptured` 发射处插入 `QElapsedTimer` 差值日志，导出 CSV 用 Python 统计
- 基线对比：DXGI Monitor 模式 vs WGC Window 模式同分辨率帧间隔对比；（待测）

**② GPU 合成单帧耗时**
- 指标：从 `AcquireNextFrame` 到 `stagingTexture->Map` 完成 < 8ms（60fps 帧预算 16.67ms 的一半）
- 工具：D3D11 Timestamp Query（`D3D11_QUERY_TIMESTAMP_DISJOINT`）或 Windows Performance Analyzer
- 注意：Map 是同步阻塞操作，建议测 P99 而非平均值；（待测）

**③ 音频端到端混音延迟**
- 指标：从 WASAPI `GetBuffer` 到对应样本被 `avcodec_send_frame` 写入 < 200ms
- 工具：在 `pushAudioData` 入口和 `avcodec_send_frame` 处各打时间戳，计算差值
- 对比：已知频率正弦信号同时播放+录制，测录制文件与源信号的相位偏移；（待测）

**④ RTMP 推流首包延迟**
- 指标：从 `startRecord` 调用到首个视频包到达服务器 < 3s
- 工具：在 `avformat_write_header` 和首次 `writePacket`（isVideo=true）处打 `QElapsedTimer` 日志；（待测）

**⑤ 摄像头预热效果**
- 指标：预热后 `onAddCamera` 到 `fireCameraAdded` 响应时间 < 100ms（vs 冷启动约 1.5s）
- 工具：在 `onAddCamera` 入口和 `EventBus::fireCameraAdded` 处各打日志时间戳；（待测）
