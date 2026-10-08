# 面经：仿OBS

> 简称：仿OBS  
> 技术栈：Qt 6.5 / FFmpeg 7 / Direct3D11 / DXGI / WGC / WASAPI / WinRT MediaCapture / C++20  
> 求职方向：音视频开发（Windows 客户端）

---

## 项目简介（简历可用，1～2 句）

基于 Qt + FFmpeg + Direct3D11 / DXGI Desktop Duplication / Windows Graphics Capture + WASAPI + WinRT MediaCapture 实现的 Windows 端一体化音视频录制与直播推流工具；支持桌面 / 窗口双模采集、多音频源混音、摄像头图层叠加、本地 MP4 录制与 RTMP 推流同步输出。

---

## 简历 Bullet（4～6 条）

- 基于 DXGI Desktop Duplication + Windows Graphics Capture 实现显示器 / 窗口双模采集，解决了 DXGI 无法捕获受 DWM 保护内容的问题
- 设计 Direct3D11 GPU 合成管线，将桌面帧 / 摄像头 / 图片图层在 GPU 端按渲染序叠加后回读，规避 CPU 逐帧混合的性能瓶颈
- 基于 WASAPI Loopback + Microphone 双路采集，使用 SwrContext + AVAudioFifo 实现多源音频重采样与帧级混音，产出单路 AAC 流
- 实现 FFmpeg 双输出管线，同时写入本地 MP4 和向 RTMP 服务器推流（FLV 封装），通过独立 PTS 序列保证推流时序连续
- 基于 WinRT MediaCapture 构建摄像头预热池，D3D 设备就绪后异步预热全部摄像头，将图层添加响应时间从约 1.5s 降至（待测）< 100ms
- 采用 EventBus 单例信号总线解耦 View / Controller，配合 QThread + QMetaObject::invokeMethod 实现采集 / 录制 / UI 三线程安全通信

---

## 面试问题（按频率排序）

---

### 主题一：视频采集（DXGI / WGC）

---

**Q1（频率：高）[采集架构]**  
**面试官问题**：你的项目同时支持显示器采集和窗口采集，为什么要用两种不同的 API？它们的适用场景和区别是什么？

**口播版（第一人称，≥150 字）**：

这是我在做这个项目时碰到的第一个坑。最开始我只用了 DXGI Desktop Duplication，发现捕获特定窗口时会拿到黑帧，比如带有版权保护的视频播放器，还有 Win11 上开启了 `WDA_EXCLUDEFROMCAPTURE` 标志的窗口。根本原因是 DXGI Duplication 捕获的是整块显示器在 DDI 层面的合成输出，DWM 对带保护标志的窗口不会把像素数据输出到 Duplication 接口。

所以我引入了 Windows Graphics Capture（WGC）作为第二后端。WGC 是 Win10 1803 引入的 WinRT API，通过 `GraphicsCaptureItem::CreateFromWindowId` 让系统以受信任身份捕获单个窗口，即使窗口被 DWM 保护也能拿到正确内容。代价是 WGC 有一个半透明的 "正在录制" 高亮边框，DXGI 没有。

我在 `DxGiCapturer` 里用 `CaptureSourceType { Monitor, Window }` 枚举控制后端选择：Monitor 模式走 `IDXGIOutputDuplication::AcquireNextFrame`，Window 模式把 `HWND` 传给 `WgcCapturer::startCapture(hwnd)` 走 WGC 的 `Direct3D11CaptureFramePool`。两者都在同一个 D3D11 设备上运行，共享 GPU 资源。

**追问 1**：DXGI AcquireNextFrame 的 timeout 参数怎么设置，设置不当会有什么后果？

**口播版（≥150 字）**：

我遇到过这个问题。`AcquireNextFrame` 的 timeout 参数单位是毫秒，传 0 表示非阻塞（帧不可用立即返回 `DXGI_ERROR_WAIT_TIMEOUT`），传正值表示最长等待多少毫秒。

如果 timeout 设得太大，比如 1000ms，那么在显示器没有刷新的情况下（比如静止画面 + 省电模式）采集线程会阻塞整整 1 秒，这段时间内所有 `QMetaObject::invokeMethod` 投递到采集线程的操作（比如切换采集目标）都会排队等待，导致 UI 响应卡死。

如果设成 0（非阻塞），则需要在代码里自己控制轮询节奏，否则在空帧时会不断空转吃满一个 CPU 核，而且产生大量无意义的 TIMEOUT 返回需要处理。

我的做法是根据目标帧率来设：60fps 对应 16ms 帧周期，我传 `timeout = 20ms`，比帧周期略长，这样在正常刷新时几乎立即返回，在静止画面时最多等 20ms 才返回 TIMEOUT，不阻塞线程过久，同时也不空转。TIMEOUT 返回时我直接 continue 跳过当前循环。

**追问 2**：在切换采集目标（比如从显示器 A 换到窗口 B）时，如何保证切换过程不崩溃、不产生花屏帧？

**口播版（≥150 字）**：

这个是我实际遇到过崩溃的场景，后来重构了采集逻辑才修好。

问题的根因是：切换时新的采集目标需要重新初始化 `IDXGIOutputDuplication`，而旧的 Duplication 对象还可能有未 Release 的锁定帧（`ReleaseFrame` 未调用），在多线程下如果 UI 线程和采集线程同时操作就会出现崩溃。

我的解决方案分三步：第一步，切换请求来自 UI 线程，我把目标存入 `m_pendingTarget`（有 mutex 保护），设置 `m_targetChanged = true`，不在 UI 线程直接操作采集对象。第二步，采集线程在每帧循环开头检查 `m_targetChanged`，如果为 true 则先调用 `ReleaseFrame` 释放当前帧锁，再 Release 旧的 Duplication 对象，然后根据新目标类型初始化对应的后端（DXGI 或 WGC），把 `m_currentTarget` 更新为 `m_pendingTarget`。第三步，切换成功后的第一帧跳过渲染（颜色可能不准），等第二帧再输出，避免花屏。这样整个切换过程全部在采集线程内完成，避免了竞态。

---

**Q2（频率：高）[WGC 窗口捕获]**  
**面试官问题**：Windows Graphics Capture 的工作原理是什么？它是如何把窗口帧拿到 D3D11 纹理里的？

**口播版（≥150 字）**：

WGC 的核心是 `GraphicsCaptureItem` + `Direct3D11CaptureFramePool` + `GraphicsCaptureSession` 三件套。

首先我通过 `GraphicsCaptureItem::CreateFromWindowId(hwnd)` 创建捕获目标；然后用我们项目的 D3D11 设备通过 `CreateDirect3D11DeviceFromDXGIDevice` 转换出 WinRT 的 `IDirect3DDevice`，这一步是桥接 WRL 和 WinRT 的关键；接着用这个 WinRT Device 创建 `Direct3D11CaptureFramePool`，指定像素格式（BGRA8）和帧池大小（我用 2 帧）。

`GraphicsCaptureSession.StartCapture()` 之后，系统会在每次窗口内容更新时，把合成后的像素异步写入帧池；我注册了 `FramePool.FrameArrived` 事件，在回调中调用 `TryGetNextFrame()` 取出最新帧，通过 `Direct3D11CaptureFrame.Surface` 拿到 WinRT `IDirect3DSurface`，再 QueryInterface 出 `IDXGISurface`，最终 QueryInterface 出 `ID3D11Texture2D`。

拿到纹理后，我把它 CopyResource 到项目内自己管理的 Staging 纹理，然后 Map 读出 CPU 端数据组装 QImage 送入编码流水线。整个过程纹理一直在 GPU 上，只有最后 Map 那一步才有 GPU→CPU 的数据传输。

**追问 1**：WGC 捕获时为什么会有黄色边框？能不能去掉？

**口播版（≥150 字）**：

这个黄色（准确来说是橙色半透明）边框是 Windows 系统的隐私保护特性，叫做 Capture Indicator，告诉用户"这个窗口正在被录制"。它是 DWM 在系统层面叠加渲染的，不是被捕获窗口自己画的，所以在捕获到的帧里是看不到这个边框的——边框只对本机用户可见，录制内容里没有。

至于能不能去掉：在 Win11 Build 22621（22H2）及以后，微软提供了 `GraphicsCaptureSession.IsBorderRequired` 属性，把它设为 `false` 就可以关闭录制边框，但这是选项而不是默认行为，而且早期 Win10 版本不支持。在我们的项目里目前没有做这个处理，因为面向的用户是开发者和测试者，不是最终消费者，所以接受了边框的存在。如果要上线给普通用户，我会在运行时检测 Windows 版本，Win11 22H2 以上则调 `IsBorderRequired = false`，否则在 UI 上提示用户"录制时会显示系统边框"。

**追问 2**：帧池大小（FramePool size）设置多少合适？设得太大或太小各有什么后果？

**口播版（≥150 字）**：

帧池大小决定了 WGC 可以预先存储多少帧，我们项目里设的是 2 帧。

如果帧池太小，比如设 1：当我们的消费端（CopyResource + Map）来不及处理时，系统没有空余帧槽来写入新帧，`FrameArrived` 回调会延迟触发或者帧被丢弃，实际表现是画面更新不流畅、帧率下降。

如果帧池太大，比如设 8：内存消耗增加（每帧按 1080p BGRA 计算约 8MB，8帧就是 64MB），更重要的是端到端延迟增加——系统可能把画面先写到队列末尾的帧槽，消费端从队列头取帧，两者之间存在多帧延迟，直播场景下主播画面会比实际滞后。

实践中 2～3 帧是合理的折中：允许采集和消费有一帧的时间差，同时不引入明显的画面延迟。我们设 2 主要是采集线程的帧处理（CopyResource + Map + QImage 构造）通常在 5ms 以内完成，不需要太大的缓冲。

---

### 主题二：Direct3D11 GPU 合成管线

---

**Q3（频率：高）[GPU 合成]**  
**面试官问题**：你说用 D3D11 在 GPU 端合成多个图层，能讲一下完整的合成流程吗？为什么不在 CPU 端用 QPainter 做？

**口播版（≥150 字）**：

这是我花时间最多的模块。首先说为什么不用 QPainter：1080p 60fps 的桌面帧是 1920×1080×4 = 约 8MB 的 BGRA 数据，每秒 60 帧就是 480MB/s 的 CPU 内存操作，加上 QPainter 的软件合成路径还要做像素级 Alpha 混合，CPU 负载会非常高，实测（待测）帧率无法稳定在 60fps。

GPU 合成的流程是：

第一步，把桌面纹理（从 DXGI 或 WGC 拿到的 `ID3D11Texture2D`）转换成 `ID3D11ShaderResourceView`（SRV），绑定到 Pixel Shader 的 `t0` 槽。

第二步，将 `m_compositionTexture`（一张和输出分辨率相同的 DEFAULT 纹理）绑定 `m_compositionRTV` 作为 RenderTarget，清空为黑色。

第三步，按 `m_renderOrder` 从底层到顶层依次调用 `renderTexture(srv, rect, isBackground)`：VS 把 rect 映射到 NDC 坐标输出一个 Quad，PS 采样对应 SRV 输出颜色，BlendState 做 Alpha 预乘混合。

第四步，所有图层叠加完毕后，`CopyResource` 到 `m_stagingTexture`（CPU_ACCESS_READ 的 STAGING 纹理），`Map` 读出字节数据构造 QImage，发 `frameCaptured` 信号投入编码队列。

这样 GPU 侧完成合成，CPU 只做最后一次 Map 和内存拷贝，大幅降低 CPU 负载。

**追问 1**：摄像头图层的纹理是怎么更新的？和图片图层有什么不同？

**口播版（≥150 字）**：

图片图层是静态的，只在图片内容改变时（比如用户替换图片）才需要更新 GPU 纹理，平时每帧直接复用同一张 `ID3D11Texture2D`，不需要任何 CPU→GPU 的数据传输，非常廉价。

摄像头图层是动态的，每帧都有新内容。`WinRTCamera` 在自己的回调线程里调用 `m_d3dContext->UpdateSubresource(m_copyTexture, ...)` 直接把 BGRA 数据上传到 GPU 纹理，并更新 `m_currentSRV`。注意这里有锁保护（`QMutex m_mutex`），因为渲染线程可能同时在读 `m_currentSRV`。

`DxGiCapturer` 在渲染摄像头图层时，从 `OverlayData::camera->getLatestFrame()` 取出最新的 SRV（这里也加锁），然后调用 `renderTexture(srv, rect)` 渲染。如果 SRV 为空（摄像头还没有产出第一帧），则跳过该图层，不会崩溃。

所以本质上：图片图层 CPU 更新频率极低（按需），摄像头图层每帧由 WinRTCamera 在自己的回调线程异步推送到 GPU，渲染线程只读 SRV，两侧通过 mutex + atomic 协调，无需手动同步 GPU 工作负载。

**追问 2**：staging texture 的 Map 操作会阻塞 CPU 多久？有没有办法优化？

**口播版（≥150 字）**：

Map 是同步操作，它等待 GPU 把所有未完成的渲染命令执行完毕（隐式 flush）后才把 staging 纹理映射到 CPU 可见地址空间。在 1080p 分辨率下，我们项目中实测（待测）Map 耗时约 2～5ms，P99 可能到 10ms，这是性能瓶颈之一。

优化方向有几个：

方向一：双缓冲 staging。用两张 staging texture 交替使用：第 N 帧把合成结果 CopyResource 到 staging[0]，同时 Map 读取上一帧 staging[1] 的数据；下一帧反转。这样 Map 的等待时间从"等本帧 GPU 完成"变成"等上一帧 GPU 完成"，通常上一帧在 Map 时已经完成了，等待时间趋近于 0。代价是延迟增加一帧（约 16ms）。

方向二：D3D11 Fence / Event Query。用 `D3D11_QUERY_EVENT` 异步检测 GPU 是否完成，不强制等待，检测到完成后再 Map，采集线程可以在等待期间做其他事情（比如处理 overlay 更新请求）。

方向三：不做 Map，直接在 GPU 端编码（NV12 纹理送 NVENC 的硬件编码器），省去 GPU→CPU→GPU 的数据搬运。我们项目目前没做这个，因为 FFmpeg 的 h264_nvenc 支持从 `AVHWFramesContext` 输入，是可行的后续优化方向。

---

### 主题三：音频采集与混音

---

**Q4（频率：高）[WASAPI 采集]**  
**面试官问题**：WASAPI 的 Loopback 模式和麦克风模式有什么区别？初始化时哪些地方容易踩坑？

**口播版（≥150 字）**：

WASAPI（Windows Audio Session API）有两种工作模式：独占模式（Exclusive）和共享模式（Shared）。我们项目用的是共享模式，因为要和其他应用共存。

Loopback 和麦克风的区别主要在设备枚举和流标志：

麦克风采集：用 `IMMDeviceEnumerator::EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, ...)` 枚举输入设备，`IAudioClient::Initialize` 时传 `AUDCLNT_STREAMFLAGS_NOPERSIST`，拿到的是麦克风的原始 PCM。

Loopback 采集：枚举的是输出设备（`eRender`），但 Initialize 时传 `AUDCLNT_STREAMFLAGS_LOOPBACK`，这样就能拿到系统正在播放的所有音频的混合输出，相当于"录屏时听到的所有声音"。

踩坑点一：Loopback 模式下，如果系统当前没有任何音频在播放，`IAudioCaptureClient::GetBuffer` 会返回 `AUDCLNT_S_BUFFER_EMPTY`，需要代码做空包处理，否则循环里会无限空转。

踩坑点二：采样率和格式是由 `IAudioClient::GetMixFormat()` 决定的，不能随意指定，通常是 48000 Hz Float32，但不同硬件可能不同；所以后续必须用 SwrContext 重采样到目标格式（我们目标是 44100 Hz），不能假设格式固定。

踩坑点三：在切换音频设备时，必须先 Stop → Release 旧的 IAudioClient，再重新初始化，否则会 HRESULT 报错 `AUDCLNT_E_DEVICE_INVALIDATED`。

**追问 1**：如果麦克风和系统音频的采样率不同，在 FFmpegRecorder 里是怎么对齐的？

**口播版（≥150 字）**：

这正是 `SwrContext + AVAudioFifo` 组合的核心价值。我们的设计是每个音频源（系统音 id=0、麦克风 id=1）在 `ensureAudioSource(id)` 时各自创建一个独立的 `SourceContext`，里面包含一个 `SwrContext` 和一个 `AVAudioFifo`。

当 `pushAudioData(sourceId, data, sampleRate, channels)` 被调用时，`resampleToFifo()` 先用该 source 的 `SwrContext`（如果采样率变了则重新初始化）把输入数据重采样到统一格式（44100Hz Float32 planar stereo），然后用 `av_audio_fifo_write` 写入该 source 的 FIFO。

编码线程驱动混音：以视频 PTS 换算出当前应编码到多少个音频样本（`av_rescale_q(lastVideoPts, {1,1000}, {1,sample_rate})`），然后 while 循环：每次从所有 source 的 FIFO 中各读出 `frame_size`（如 1024）个样本加法混音到 `mixBufferL/R`，调 `applyFadeEffect` 应用淡入淡出增益，`std::clamp` 防止削波，送 `avcodec_send_frame`。

这样无论麦克风是 48000Hz 还是 44100Hz，都在各自的 SwrContext 里被归一化，最终在 FIFO 出口是完全对齐的格式，混音加法不会因采样率不同产生偏移。

**追问 2**：音频 Fade 淡入淡出是怎么实现的？为什么放在编码线程而不是采集线程？

**口播版（≥150 字）**：

`applyFadeEffect` 的实现原理是对 `mixBufferL/R` 里的每个样本乘以一个 0.0～1.0 的增益系数 `gain`：

淡入阶段：`gain = (float)currentSamplePos / (float)fadeInSamples`，currentSamplePos 从 0 开始线性增长到 fadeInSamples，gain 从 0 线性增长到 1.0，这就是标准的线性淡入。

淡出阶段：当 `m_softStopRequested` 被设为 true 时，记录 `fadeOutStartSample = m_totalAudioSamples`，之后每个样本的 `gain` 从 1.0 线性衰减到 0：`gain = 1.0 - (float)(currentPos - fadeOutStartSample) / (float)fadeOutSamples`。等到 `m_totalAudioSamples >= finalAudioSampleEnd` 且视频队列也空了，录制线程才真正退出。

为什么放在编码线程而不是采集线程：采集线程（WasapiCapturer）不知道录制的时间基，也不知道视频 PTS，无法准确计算样本偏移；而编码线程有 `m_totalAudioSamples` 这个全局计数，能精确知道每个样本对应录制开始后的第几个样本，增益计算准确。如果放在采集侧还会导致数据被修改后入 FIFO，和其他 source 混音时 gain 不一致。

---

### 主题四：FFmpeg 录制与 RTMP 推流

---

**Q5（频率：高）[双输出管线]**  
**面试官问题**：你怎么实现同时录制本地文件和推流直播的？一份编码数据怎么写给两个输出？

**口播版（≥150 字）**：

我的做法是共用一个编码器（`AVCodecContext`），但维护两个独立的 `AVFormatContext`：`m_fmtCtxFile`（本地 MP4）和 `m_fmtCtxStream`（RTMP FLV）。

编码流程是：原始帧（QImage）→ SwsContext 色彩空间转换为 YUV420P → `avcodec_send_frame` 送给编码器 → `avcodec_receive_packet` 循环取出编码包（`AVPacket`）→ `writePacket(pkt, isVideo=true)` 函数内分别给文件和推流各自做时间基转换后写入。

`writePacket` 的关键在于 PTS 转换：文件流和推流的 `AVStream::time_base` 不同（文件通常 1/90000，FLV/RTMP 通常 1/1000），所以每个包在写入前必须用 `av_rescale_q(pkt->pts, encoderTimeBase, streamTimeBase)` 转换，还要确保写入推流的 PTS 严格单调递增，所以我用 `m_lastStreamVideoPts/Dts` 跟踪上一包的值，异常时做修正。

初始化时，文件侧用 `avio_open` + `avformat_write_header`，推流侧用 `avio_open2` 并传入 RTMP 专用参数（`rtmp_transport=tcp`、`tcp_nodelay=1`、`rw_timeout=5000000`）。两路输出同步初始化，任意一路失败不影响另一路继续工作（降级处理）。

**追问 1**：RTMP 推流时 FLV 封装有哪些特殊要求？你踩过哪些坑？

**口播版（≥150 字）**：

FLV 封装对 H264 视频有几个特殊要求，我都踩过：

第一个坑：`codec_tag` 必须设为 0。我把编码器的 `codecpar` 复制到推流 AVStream 后，FLV muxer 对某些 codec 有自己的 tag 映射，如果不把 `m_streamVStream->codecpar->codec_tag = 0` 则 muxer 可能报 "Tag … incompatible with output format" 警告，甚至写入失败。设 0 让 muxer 自动选择正确的 tag。

第二个坑：`avformat_alloc_output_context2` 必须显式指定格式名为 `"flv"`，不能让 FFmpeg 从 URL 后缀推断，因为 `rtmp://` 开头的 URL 不带文件后缀，推断会失败或用错格式。

第三个坑：`avformat_write_header` 之后才能调 `avcodec_receive_packet`，否则第一个包的 extradata（SPS/PPS）没有写入，推流端解码器找不到关键帧信息，黑屏或花屏。

第四个坑：RTMP 连接在弱网下可能 `avio_open2` 卡住很久，所以必须传 `rw_timeout` 参数；我设的是 5 秒，超时后返回错误，上层提示用户推流失败，而不是无限卡死。

**追问 2**：H264 编码器选择了哪些？不同编码器对参数有什么影响？

**口播版（≥150 字）**：

`RecorderConfig` 里 `encoderName` 支持三种：`h264_nvenc`（NVIDIA 硬件编码）、`h264_qsv`（Intel Quick Sync，集显）、`libx264`（CPU 软编）。

NVENC 和 QSV 都是硬件编码器，特点是 CPU 占用极低（编码负载在 GPU / iGPU 上），适合高帧率场景，但画质在同码率下略低于 libx264，且设置参数时有自己的约定：NVENC 用 `rc:v cbr`（固定码率）比较适合直播，libx264 用 `crf` 控制质量更灵活。

踩坑点：NVENC 要求输入帧必须是 YUV420P 或 NV12，我们的 `SwsContext` 目标格式要和编码器的 `pix_fmt` 一致；libx264 的 `preset` 影响编码速度和 CPU 占用，直播场景选 `ultrafast` 或 `superfast`，否则 CPU 跟不上 60fps 实时编码。

运行时如果指定的编码器不可用（比如没有 NVIDIA GPU），我们目前不做自动降级（待优化），直接报错让用户在设置里切换。生产环境应该在启动时探测可用编码器，按 NVENC > QSV > libx264 优先级自动选择。

---

**Q6（频率：高）[PTS 时序]**  
**面试官问题**：音视频同步是怎么做的？如果音频数据比视频来得早或晚，怎么处理？

**口播版（≥150 字）**：

我们项目的音视频同步策略是以视频 PTS 为基准驱动音频编码，而不是各自独立计时。

具体做法：DxGiCapturer 在 `frameCaptured` 信号里带上一个 `qint64 timestamp`（毫秒级，由 `QElapsedTimer` 从录制开始计时），这个 timestamp 就是视频帧的 PTS。录制线程收到视频帧后记录 `lastVideoPts`，然后驱动音频编码循环：`targetSamples = av_rescale_q(lastVideoPts, {1,1000}, {1,sampleRate})`，只要 `m_totalAudioSamples < targetSamples` 就持续从 FIFO 取样本编码音频帧，保证音频样本数与视频时间基对齐。

如果音频来得比视频早（FIFO 里有大量积压）：编码线程会快速消费完积压的音频帧直到 totalAudioSamples 追上 targetSamples，不会产生长延迟。

如果音频来得晚（FIFO 里样本不足 frame_size）：我的策略是用零样本补充——`av_audio_fifo_size(fifo) < frame_size` 时，不从该 FIFO 读数据，`mixBufferL/R` 对应部分保持全零，相当于静音。这会导致轻微的音频时间戳轻微超前于实际内容，但比卡等样本更稳定，也不会导致 PTS 跳变。

RTMP 推流时音频和视频的 DTS 也需要交织（interleave），`av_write_frame` 会根据 DTS 自动排序，所以两路的时间基转换必须一致。

---

### 主题五：WinRT 摄像头与预热池

---

**Q7（频率：中）[摄像头预热]**  
**面试官问题**：你提到用摄像头预热池减少添加延迟，为什么 MediaCapture 初始化会这么慢？预热的时机怎么选？

**口播版（≥150 字）**：

`MediaCapture.InitializeAsync()` 慢有几个原因：首先要向系统请求摄像头权限（UWP 隐私权限），系统要弹出确认或检查已有权限；其次要枚举摄像头的所有支持格式（`GetAvailableMediaStreamProperties`），一些摄像头格式列表很长；最后还要协商并设置最终格式（`SetMediaStreamPropertiesAsync`）并启动 `MediaFrameReader`。整个过程在我的机器上实测约 800ms～1.5s，用户体验明显卡顿。

预热的思路是：不等用户点"添加摄像头"时再初始化，而是在 D3D 设备初始化完成后立即异步预热所有已知摄像头，把初始化好的 `WinRTCamera` 实例存入 `m_cameraPool`（`QMap<QString, WinRTCamera*>`，以 DeviceId 为 key）。

预热时机的选择：必须等 D3D 设备 `init()` 完成后，因为 `WinRTCamera::init(ID3D11Device*)` 需要 D3D 设备来创建 WinRT IDirect3DDevice 桥接；D3D 设备在 `DxGiCapturer::initFinished(true)` 信号发出后确保可用。所以我在 `onCapturerInitFinished(true)` 回调中调用 `prewarmAllCameras()`，用 `QMetaObject::invokeMethod` 在采集线程（持有 D3D 设备的线程）内完成 WinRTCamera 的 init + start。

用户点"添加摄像头"时，`onAddCamera` 先查 `m_cameraPool`，命中则直接调 `addCameraOverlay`，响应时间从约 1.5s 降至（待测）< 100ms。

**追问 1**：WinRTCamera 的格式协商为什么 BGRA8 得分最高？NV12 不是更节省带宽吗？

**口播版（≥150 字）**：

从网络或存储带宽角度看 NV12 确实更节省——NV12 是 YUV420 半平面格式，每像素 1.5 字节，而 BGRA8 每像素 4 字节，带宽只有 BGRA 的 37.5%。但这里的"带宽"是摄像头→驱动→应用的系统内存带宽，不是网络带宽。

在我们的使用场景里，摄像头帧最终要上传到 GPU 并用 Shader 合成到桌面帧上。如果拿到 NV12 帧，需要额外的颜色空间转换（YUV→RGB）才能合成，选项有两个：一是 CPU 端软件转换后上传 BGRA，二是 GPU Shader 里做 NV12→RGB 转换（需要额外的 Shader 分支）。

而 BGRA8 可以直接上传到 `DXGI_FORMAT_B8G8R8A8_UNORM` 纹理，无需任何转换，直接用标准 Pixel Shader 采样就能在合成时输出正确颜色。`SoftwareBitmap::Convert(BGRA8, Premultiplied)` 虽然有 CPU 开销，但它内部用 WIC 利用 SIMD 指令集加速，实际开销远小于手写的 YUV→RGB 软件转换，而且逻辑最简单、最稳定。

所以综合考虑"到 GPU 合成的路径最短、代码最简单"，BGRA8 是最优选择，NV12 放第二，YUY2 第三，MJPG 最差（还需要 JPEG 解码）。

**追问 2**：如果同一个摄像头同时被多个 WinRTCamera 实例 open 会怎样？SharedReadOnly 是什么？

**口播版（≥150 字）**：

这正是我们用 `SharingMode::SharedReadOnly` 的原因。默认 `MediaCapture` 以独占模式打开摄像头，意味着同一时刻只有一个进程/会话能访问摄像头，其他人会收到 "设备已被使用" 的错误。

`MediaCaptureSharingMode::SharedReadOnly` 是 Win10 1803+ 引入的共享模式，允许多个 `MediaCapture` 实例以只读方式同时访问同一摄像头，系统在驱动层面做帧广播。实际使用中，这意味着我们可以同时从系统摄像头 app 和我们的应用各自读取摄像头帧，互不干扰。

在预热池场景下，`SharedReadOnly` 还有另一个好处：预热时已经 start 了一个 WinRTCamera 实例，用户后来又想再开一个摄像头图层（理论上同 deviceId 的第二个图层），因为是 SharedReadOnly 所以不会因为"独占冲突"而失败。当前代码里我用 DeviceId 做去重（`m_activeLayers` 里检查同 deviceId 是否存在），所以同一摄像头不会被添加两次，但 SharedReadOnly 为这种场景预留了技术可能性。

---

### 主题六：线程架构与 EventBus

---

**Q8（频率：高）[多线程架构]**  
**面试官问题**：这个项目有几个线程？各自职责是什么？线程间的数据是怎么传递的？

**口播版（≥150 字）**：

项目里主要有四个线程：

**主线程（UI 线程）**：运行 Qt 事件循环，负责 `MainWindow` 渲染和用户交互。所有 Qt Widget 操作必须在主线程，所以 EventBus 的 `state_*` 信号（如 `data_previewFrame`）通过 `Qt::QueuedConnection` 投递到主线程处理。

**视频采集线程（m_videoThread）**：运行 `DxGiCapturer`，负责 DXGI/WGC 采集 + D3D11 GPU 合成 + staging readback。这个线程持有 D3D11 Device Context，所有 D3D 操作必须在这个线程内执行；WinRTCamera 的预热也在这个线程完成（通过 `invokeMethod` 投递的 lambda）。

**录制线程（FFmpegRecorder，继承 QThread）**：运行编码和混音循环，消费 `m_videoQueue` 和 `m_audioQueue`，输出编码包到文件和 RTMP。

**音频采集线程（WasapiCapturer，两个实例分别继承 QThread）**：麦克风线程和系统音线程各自独立，通过 `audioDataReady` signal（QueuedConnection）把 PCM 数据投递到录制线程。

数据传递规则：跨线程的大数据（QImage、QByteArray）通过 Qt Signal（QueuedConnection，隐式 copy）或带锁的生产者-消费者队列传递；跨线程的控制指令通过 `QMetaObject::invokeMethod` 投递到目标对象所在线程执行，避免数据竞争。

**追问 1**：为什么用 QMetaObject::invokeMethod 而不是直接调用函数？

**口播版（≥150 字）**：

直接跨线程调用函数会产生数据竞争。`DxGiCapturer` 里的 D3D 资源（ComPtr 成员、overlay Map）只在视频采集线程操作，如果 UI 线程直接调用 `m_capturer->removeOverlay(id)`，两个线程同时操作 `m_overlays` 这个 `QMap` 会导致未定义行为（崩溃或数据损坏）。

`QMetaObject::invokeMethod(m_capturer, "removeOverlay", Qt::QueuedConnection, Q_ARG(int, id))` 的机制是：把一个"调用 removeOverlay(id)"的消息投入 `m_capturer` 所在线程（即视频采集线程）的事件队列；视频采集线程在自己的事件循环里取出并执行，这样 `removeOverlay` 始终在正确的线程里运行，没有竞态。

`Qt::BlockingQueuedConnection` 则是同步版本——调用方线程会阻塞等待目标线程执行完函数后才继续，用于需要立即生效的操作，比如 `stopCapture` 必须等采集线程真正停止后才能销毁 D3D 资源，我在 `stopVideoThread()` 里用的就是 `BlockingQueuedConnection`。

代价是：BlockingQueuedConnection 容易死锁——如果目标线程正在等待调用方线程（循环等待），就会卡死。所以要非常谨慎地控制使用场景。

**追问 2**：EventBus 的 Command / Data / State 三类信号怎么划分的？这样设计有什么好处？

**口播版（≥150 字）**：

这三类信号对应 MVC 里不同方向的数据流：

**Command（View → Controller）**：用户操作触发，如 `cmd_changeSource`、`cmd_toggleRecording`、`cmd_addCamera`。MainWindow 调 EventBus 的 `sendCommand*` 方法发射这些信号，SystemController 连接并处理。这些信号代表"用户想做什么"，是指令而不是数据。

**Data（Controller → View，高频）**：如 `data_previewFrame`（每帧一次）、`data_overlayUpdate`、`data_audioLevel`。这类信号频率最高，直接带数据负载（QImage、float），View 层订阅后更新 UI 显示。之所以单独分类，是为了让 View 层知道"这些信号是数据流，要考虑节流或丢帧处理"。

**State（Controller → View，低频）**：如 `state_recordingChanged`、`state_durationUpdated`、`state_error`。这类是状态变更通知，频率低，触发特定的 UI 状态切换（按钮状态、错误弹窗）。

好处有三点：第一，依赖关系清晰——View 不持有 Controller 的指针，只通过 EventBus；第二，可观测性好——只需在 EventBus 信号上打断点或加日志，就能追踪所有跨层交互；第三，替换后端方便——如果把 DxGiCapturer 换成其他采集后端，只要保持 EventBus 接口不变，View 层零改动。

---

**Q9（频率：中）[崩溃处理与日志]**  
**面试官问题**：你的项目里有 CrashHandler 和 Logger，它们是怎么实现的？为什么需要它们？

**口播版（≥150 字）**：

这两个是我在项目早期频繁遇到崩溃后加的基础设施。

`Logger` 是一个简单的单例，将 `qDebug/qWarning/qCritical` 的输出通过 `qInstallMessageHandler` 重定向到文件（`app_log.txt`），并加上时间戳和线程 ID。这样在 Release 模式下（没有 VS 调试器挂载）也能看到崩溃前最后一条日志，缩短定位时间。实际开发中最有用的场景是：采集线程崩溃时打印了崩溃前的 D3D 错误码，我才发现是 `AcquireNextFrame` 拿到了坏帧导致后续 CopyResource 失败。

`CrashHandler` 的作用是在程序崩溃（未捕获异常、访问违规）时生成 minidump 文件，用 `SetUnhandledExceptionFilter` 注册全局异常处理器，在里面调用 `MiniDumpWriteDump` 写出 .dmp 文件。有了 .dmp 和对应的 .pdb，可以在 VS 或 WinDbg 里加载，看到崩溃时的调用栈和变量状态，即使没有复现环境也能定位问题。

为什么需要它们：音视频应用涉及大量 D3D / COM / WinRT 操作，这些 API 失败时不一定抛异常，有时是返回错误 HRESULT 被忽略，有时是在驱动层面崩溃。有日志和 minidump，可以从现场证据反推根因，否则只能靠复现，效率极低。

---

**Q10（频率：中）[背压与帧丢弃]**  
**面试官问题**：如果编码速度跟不上采集速度，你的系统会怎么处理？会不会内存爆掉？

**口播版（≥150 字）**：

这是我专门设计了背压机制来应对的场景。

视频方向：`FFmpegRecorder::pushVideoFrame()` 在加锁后检查 `m_videoQueue.size() > 150`，超过直接 return 丢弃当前帧。150 帧在 30fps 下是 5 秒的缓冲，足够应对短时编码卡顿；超过说明编码长期跟不上，继续入队只会让内存无限增长最终 OOM。丢帧的后果是录制文件出现跳帧（时间戳不连续），但这比 OOM 崩溃要好得多，且短时编码抖动下 150 帧缓冲足以平滑掉。

UI 预览方向：`DxGiCapturer` 里用 `m_uiInFlight` 原子计数控制 preview 帧的节奏：每次发 `frameCapturedForPreview` 前把 `m_uiInFlight` 加 1，UI 线程处理完后通过 `cmd_uiFrameProcessed` 通知采集线程将 `m_uiInFlight` 减 1；采集线程在 `m_uiInFlight > 0` 时跳过 preview 发射，避免 UI 线程来不及渲染时的帧积压。

音频方向：`AVAudioFifo` 本身是弹性缓冲，不做硬限制；但编码线程是主动消费端，只要视频帧在流动，音频消费就在持续进行，不会无限积压。极端情况下（编码线程卡死）音频 FIFO 会增长，但这时整个系统已不正常，属于需要排查根因的情况，不是背压能解决的问题。

---

## 模拟面试 · 综合问题（新增，可直接口播）

> 这 10 道是真实面试里最常问的综合题，把前面 Q1–Q10 的技术点串起来考。以下答案均为第一人称口播，可直接背诵。

---

### 综合 1：整体架构与 EventBus 解耦

**面试官问题**：简单介绍这个项目整体架构，EventBus 是怎么实现的？怎么做到 UI、采集、编码线程解耦？为什么要解耦，如果不解耦会出现什么问题？

**口播版（第一人称，≥150 字）**：

这个项目是一个仿 OBS 的 Windows 端音视频录制/推流工具，整体是 Qt 的 MVC 分层 + 多线程生产者消费者结构。

线程上一共有四个：主线程跑 Qt 事件循环负责 UI；一个视频采集线程跑 `DxGiCapturer`，持有唯一的 D3D11 Device 和 DeviceContext，负责 DXGI/WGC 采集、GPU 合成和回读；一个录制线程就是 `FFmpegRecorder`（继承 QThread），跑编码和混音循环；另外两个独立的 `WasapiCapturer` 线程分别采集麦克风和系统声音。

解耦靠的是 `EventBus`。它是一个 `QObject` 单例，把所有跨层交互抽象成三类信号：Command（`cmd_*`，View→Controller 的指令，比如 `cmd_toggleRecording`）、Data（`data_*`，Controller→View 的高频数据流，比如 `data_previewFrame`）、State（`state_*`，低频状态通知，比如 `state_recordingChanged`）。MainWindow 只调用 `EventBus::instance()->sendCommandXxx()` 发指令、订阅 `data_*`/`state_*` 更新 UI；SystemController 在 `initialize()` 里把 `cmd_*` 连到自己的槽函数。这样 View 根本不持有 Controller 指针，两端只依赖 EventBus 这一个契约。

为什么要解耦：采集、编码都是阻塞和耗时操作（D3D 的 Map、FFmpeg 的 `avcodec_receive_packet`），如果不分线程，UI 线程会被卡死整个界面无响应；如果不通过 EventBus 而让各模块直接互相调用，就会有多线程同时操作同一份状态（比如 `m_overlays` 这个 QMap）的竞态，表现为随机崩溃或数据损坏。EventBus + QueuedConnection 把跨线程调用都变成投递到目标线程事件队列的消息，天然串行化，把并发问题收敛到"队列"这一处。

---

### 综合 2：DXGI 与 WGC 两种采集方案

**面试官问题**：DXGI Desktop Duplication 和 Windows Graphics Capture 两种采集方案区别是什么？各自优缺点，什么场景用哪一个？

**口播版（第一人称，≥150 字）**：

两者都是官方提供的屏幕捕获技术，但捕获层级完全不同。

DXGI Desktop Duplication（`IDXGIOutputDuplication`）是显卡驱动层面的接口，它拿到的是 DWM 把整块显示器合成之后的桌面画面，性能极高、延迟最低，还能顺带拿到鼠标指针和 dirty region 的元数据。但它的缺点正好来自"整屏合成"这一点：凡是 DWM 不输出到 Duplication 接口的内容都拿不到，典型就是带 DRM 保护的视频播放器、以及 Win11 上设了 `WDA_EXCLUDEFROMCAPTURE` 的窗口，捕获出来是黑帧。而且它必须和输出同一个 GPU 的 Adapter 创建 D3D 设备，跨显卡/切显示器要重建。

Windows Graphics Capture 是 Win10 1803 引入的 WinRT API，通过 `GraphicsCaptureItem::CreateFromWindowId` 让系统以受信任身份去捕获**单个窗口**，即使窗口被 DWM 保护也能拿到正确内容。代价是：一，它有一个橙色的"正在录制"系统边框（录制内容里没有，但对本机用户可见）；二，它捕获的是单个窗口而不是整屏，多窗口要分别捕获；三，接口是 WinRT 异步模型，落地到 D3D 纹理要经过 `IDirect3DDxgiInterfaceAccess` 桥接。

我的项目里用 `CaptureSourceType { Monitor, Window }` 枚举在 `DxGiCapturer` 里统一调度：选显示器走 DXGI 的 `AcquireNextFrame`（我设 timeout 100ms，既不在静止画面下长时间阻塞采集线程，也不空转吃满 CPU）；选窗口就把 HWND 交给 `WgcCapturer`，走 `Direct3D11CaptureFramePool` + `GraphicsCaptureSession`。两者共用同一个 D3D11 设备，帧拿到后统一进 GPU 合成管线。结论：录整屏/性能优先用 DXGI，录指定窗口/要绕保护用 WGC。

---

### 综合 3：WASAPI 采集与双路混音

**面试官问题**：WASAPI 做系统音频回采，讲讲采集流程，麦克风和系统声音两路混音你是怎么做的？设备热切换如何处理？会遇到什么问题？

**口播版（第一人称，≥150 字）**：

采集流程：每个 `WasapiCapturer`（继承 QThread）在自己的 `run()` 里先 `CoInitialize`（必须用 STA，我之前用 MTA 直接把 Qt 主线程搞崩过），然后 `CoCreateInstance` 出 `MMDeviceEnumerator`，拿到设备后用 `Activate(__uuidof(IAudioClient))` 激活，`GetMixFormat()` 拿到系统规定的格式（一般是 48000Hz Float32，但不能假设），`Initialize(AUDCLNT_SHAREMODE_SHARED, flags, ...)` 初始化——麦克风 flags 传 0，系统回采传 `AUDCLNT_STREAMFLAGS_LOOPBACK`。之后 `GetService(IAudioCaptureClient)`，进循环：`GetNextPacketSize` 看有没有数据，有就 `GetBuffer`，把 PCM 统一转成 Float32，顺便算 RMS 音量发 `volumeLevelChanged`，再把原始数据 `emit audioDataReady(data, rate, channels)` 投给录制线程。空包（`AUDCLNT_S_BUFFER_EMPTY`）就 `msleep(5)` 跳过，不空转。

混音在 `FFmpegRecorder` 里做。系统音 sourceId=0、麦克风 sourceId=1，各自有独立的 `SourceContext`（一个 `SwrContext` + 一个 `AVAudioFifo`）。`pushAudioData` 先把数据入 `m_audioQueue`，录制线程取出来调 `resampleToFifo`，用各自的 SwrContext 把任意采样率重采样到统一的 44100Hz Float32 planar，写进 FIFO。编码时每次读 `frame_size`（1024）个样本，把两路做**加法混音**到 `mixBufferL/R`，再做 `std::clamp` 防削波、`applyFadeEffect` 做淡入淡出，最后 `avcodec_send_frame` 给 AAC。因为两路在 FIFO 出口已经是同一格式同一采样率，加法不会因采样率不同产生错位。

设备热切换：我封装了 `restartAudio(isMic, deviceId)`，就是 stop → wait → delete 旧实例 → new 一个新的 `WasapiCapturer` → `setDevice` → 重新 connect → start。这里最大的坑是直接换设备会报 `AUDCLNT_E_DEVICE_INVALIDATED`，必须整条链路重建，不能只换 `IAudioClient`。诚实地讲，我目前是用户手动触发 `cmd_refreshDevices` 去刷新+重建，还没有注册 `IMMNotificationClient` 做系统级的即插即用通知，这是我列在优化清单里的一条。

---

### 综合 4：WinRT 摄像头采集与多图层叠加渲染

**面试官问题**：WinRT Camera + D3D11 摄像头采集，多图层叠加渲染，讲讲渲染流程，图层的顺序、透明度怎么处理？

**口播版（第一人称，≥150 字）**：

摄像头采集这块我用 WinRT MediaCapture 做摄像头获取，初始化的时候设置共享模式，避免别的程序占用摄像头。会做格式协商，优先选 BGRA8 格式，方便后续 D3D 渲染。拿到帧之后，转成预乘 Alpha 的 BGRA 图像，上传成 D3D 纹理，多线程下用互斥锁保护最新纹理对象，供渲染线程读取。

合成渲染流程跑在 D3D11 渲染线程里。首先清空渲染目标，设置好着色器、采样器和混合状态。先绘制桌面 / 窗口作为背景层；之后遍历所有业务图层，包括摄像头、图片、文字图层，把每个图层的坐标转换到 NDC 空间，逐个绘制四边形 Quad。全部合成完成后，把 GPU 纹理拷贝到暂存资源，回读到 CPU 内存，抛信号给上层做预览或者编码。

图层顺序：我维护一个图层渲染顺序列表，UI 拖拽调整层级时，把新的顺序下发给渲染线程，渲染的时候按照从底层到顶层依次绘制，后画的图层会覆盖前面的。

透明度依靠 D3D 的混合状态，使用标准源 Alpha 混合公式。所有输入图像统一转成**预乘 Alpha**格式，保证半透明边缘渲染正确。做了空纹理保护，如果图层还没有有效帧，就跳过绘制，防止程序崩溃。

### 综合 5：三种编码器对比与帧数据传递

**面试官问题**：FFmpeg 部分，NVENC/QSV/libx264 三种编码器简单对比，项目中编码器切换逻辑怎么做？编码线程和采集线程之间怎么传递帧数据？

**口播版（第一人称，≥150 字）**：

三种编码器的本质区别在算力放哪：`h264_nvenc` 是 NVIDIA 独显的硬件编码器，`h264_qsv` 是 Intel 核显的 Quick Sync，两者编码负载在 GPU/iGPU 上，CPU 占用极低，适合高帧率实时；`libx264` 是 CPU 软编，同码率画质最好、参数最灵活，但吃 CPU。硬件编码器有一个硬约束：输入像素格式必须是 NV12，所以我在 `initVideoEncoder` 里根据编码器名设 `pix_fmt`——nvenc/qsv 用 `AV_PIX_FMT_NV12`，libx264 用 `AV_PIX_FMT_YUV420P`，SwsContext 的目标格式要跟着它走，否则编码器拒收。

切换逻辑：`RecorderConfig.encoderName` 由用户在设置里选，`initVideoEncoder` 先 `tryOpenCodec(配置的编码器)`，失败了就降级 `tryOpenCodec("libx264")` 兜底（保证没独显的机器也能跑）。libx264 我固定 `preset=ultrafast` + `tune=zerolatency` 保证实时。这里一个诚实的短板是：目前只有"硬件编码器失败→libx264"这一层降级，没有做 NVENC→QSV→libx264 的完整优先级自动探测，是我后续要补的。

帧数据传递：采集线程合成回读出 QImage 后 `emit frameCaptured(img, timestamp)`，通过 `Qt::QueuedConnection` 投到 SystemController，`onVideoFrameCaptured` 里直接调 `m_recorder->pushVideoFrame`。`pushVideoFrame` 内部用 `QMutex` 保护一个 `QQueue<VideoFrameData>`，先统一转成 RGBA8888 深拷贝再入队；注意 QueuedConnection 本身就隐含一次 QImage 的拷贝，所以我只在入队时再做一次显式 copy 保证生命周期。如果队列超过 150 帧就直接丢帧（背压），防止编码跟不上时内存无限涨。

---

### 综合 6：时间戳单调性

**面试官问题**：你提到时间戳单调性问题，音视频录制 / 推流中时间戳乱会造成什么现象？你怎么保证时间戳正确？

**口播版（第一人称，≥150 字）**：

时间戳乱分两种情况：回退（后一帧 PTS 比前一帧小）和跳变（突然跳很大或变成负的）。在推流里，FLV/RTMP 协议要求时间戳单调递增，一旦回退，RTMP 服务器会立刻断开连接（实测返回 `-10053` WSAECONNABORTED），直播直接断流；本地 MP4 里时间戳乱会表现为播放器花屏、跳帧、进度条回跳，音画不同步。所以时间戳是我单独做了强制矫正的地方。

保证正确的三层：第一层，源头就单调——视频 PTS 不依赖系统时钟，而是用采集帧计数器 `s_recordedFrameCount * 1000 / TARGET_FPS` 生成，每帧严格递增；音频 PTS 用全局样本计数 `m_totalAudioSamples` 生成，也是单调的。第二层，统一换算——写入前每个包都用 `av_packet_rescale_ts` 从编码器 time_base 换到目标流 time_base，文件流和推流各自独立换算。第三层，兜底矫正——`writePacket` 里对推流维护 `m_lastStreamVideoDts/Pts`，如果新包的 dts 小于上次的就强制 `= lastDts + 1`，如果 pts 小于 dts 就 `pts = dts`（保证 PTS >= DTS），彻底杜绝回退。音视频同步则反过来用：以视频 PTS 为基准换算当前该编码到多少个音频样本，驱动音频编码去追，而不是让两路各自独立计时。

---

### 综合 7：窗口切换问题、崩溃恢复与跨线程坑

**面试官问题**：窗口切换的时候会遇到哪些问题？崩溃恢复你做了哪些机制？跨线程通信你踩过哪些坑？

**口播版（第一人称，≥150 字）**：

窗口切换是我真实踩过崩溃的地方。切换采集目标（显示器↔窗口、换显示器）时，旧后端可能还有没 `ReleaseFrame` 的锁定帧、没 Release 的 Duplication 对象，如果 UI 线程直接去动采集对象，或者旧的 GPU 纹理还挂在 SRV 上，就会崩溃或花屏。我的解法是"请求-确认"两段式：UI 线程只把目标写进 `m_pendingTarget` 并置 `m_targetChanged`（mutex 保护），采集线程在每帧循环开头检查，发现变了就在自己线程里 `resetCaptureState()` 释放旧的 Duplication/WGC session 和纹理，再按新目标初始化对应后端，切换后第一帧跳过渲染避免颜色不准。整个切换只在采集线程内发生，没有竞态。

崩溃恢复分三层：一是 `CrashHandler`，用 `SetUnhandledExceptionFilter` 注册全局异常处理器，崩溃时 `MiniDumpWriteDump` 生成 .dmp（配 .pdb 能在 WinDbg 里看调用栈，不需要复现环境）；二是 `Logger`，把 qDebug 重定向到文件、带毫秒时间戳和线程 ID，Release 下也能看到崩溃前最后一条日志；三是运行时自愈，DXGI 的 `DXGI_ERROR_ACCESS_LOST`（比如显示器切换、锁屏）我会自动 `resetCaptureState` 重建，不崩溃。

跨线程踩过的坑主要有三个：一是 COM 初始化，WASAPI 线程必须 `CoInitialize`（STA），我之前用 MTA 把 Qt 主线程带崩了；二是 `QMetaObject::invokeMethod` 的 `BlockingQueuedConnection` 会死锁，只在 `stopVideoThread` 这种必须同步等采集线程停的地方用，其余全部 QueuedConnection；三是 QueuedConnection 传大 QImage 有隐式拷贝开销，我后来用 `m_uiInFlight` 原子计数做预览帧节流，避免 UI 来不及消费时帧在队列里堆积。

---

### 综合 8：RTMP 容错与音视频同步

**面试官问题**：RTMP 推流，网络抖动、断网你做了什么容错处理？有没有做音视频同步？怎么做的？

**口播版（第一人称，≥150 字）**：

容错分"连接时"和"写入时"两段。连接时我在 `avio_open2` 传了 RTMP 专用参数：`rtmp_transport=tcp` 强制走 TCP、`tcp_nodelay=1` 禁用 Nagle 降低延迟、`rw_timeout=5000000` 设 5 秒读写超时——这是关键，否则弱网下 `avio_open2` 会无限卡住，UI 直接假死；还设了 `rtmp_buffer=1000` 和 4MB 的 socket 发送缓冲抗抖动。写入时我在 `writePacket` 里检查 `av_interleaved_write_frame` 的返回值，如果是 `-10053`（WSAECONNABORTED）或 `EPIPE` 说明连接已死，我就 `avio_closep` + `avformat_free_context` 把推流输出置空，**但本地 MP4 录制继续**，两路输出降级而不是一起挂。诚实的短板是：目前断了之后只是停掉推流，没有做断线自动重连重推，这是我要补的。

音视频同步用"视频 PTS 驱动音频"的策略。每次收到视频帧记录 `lastVideoPts`，然后 `targetSamples = av_rescale_q(lastVideoPts, {1,1000}, {1,sampleRate})`，只要 `m_totalAudioSamples < targetSamples` 就持续从 FIFO 取样本编码音频帧，保证音频样本数跟视频时间基对齐。音频来得早就快速消费积压、来晚就用零样本补静音，不会卡等。最终 `av_interleaved_write_frame` 会按 DTS 自动交织两路，前提是前面第 6 题说的 time_base 换算和单调性必须做对。

---

### 综合 9：内存泄露与内存暴涨排查

**面试官问题**：项目有没有遇到内存泄露、内存暴涨问题？你用什么工具排查（如 vs 调试、valgrind、windbg），怎么定位解决？

**口播版（第一人称，≥150 字）**：

先纠正一下：valgrind 是 Linux 的，Windows 上我用的是另一套。主要工具是 Visual Studio 的诊断工具（跑起来打两个内存快照，对比对象数量增长定位泄露点）、WinDbg 配 gflags 的 user-mode dump 看堆分配栈，还有项目自带的 `CrashHandler` 生成的 minidump 和 `Logger` 日志。COM 相关的泄露还可以用 Application Verifier 或看引用计数。

这个项目里我重点防的是两类：一是 COM/WinRT 对象的泄露，这部分我全部用 WRL `ComPtr` 智能指针管，`SAFE_RELEASE` 宏兜底，生命周期基本不用操心；二是自己 `new` 出来的裸指针——`OverlayData` 和 `CameraContext` 是手动管理，`OverlayData` 在 `removeOverlay` 里进 `m_pendingRemovals` 队列、由 `processPendingUpdates` 统一释放，`CameraContext` 在 `cmd_removeOverlay` 处理里 delete，`cleanupCameraPool` 里统一清理，这些是我重点排查过、保证不漏的地方。

内存暴涨方面，主要靠背压和上限控制：视频队列超过 150 帧就丢帧；音频虽然 FIFO 是弹性的，但录制线程是主动消费端，只要视频在流动音频就持续消费，不会无限积压。切换采集目标时 `resetCaptureState` 会释放旧的 Duplication 和纹理，避免频繁切换把显存和内存越堆越高。定位方法上，我习惯在关键资源的创建/销毁处打 `LOG_INFO`，配合时间戳和线程 ID，出问题时翻日志就能看到是哪个对象的生命周期没对上。

---

### 综合 10：项目优化方向

**面试官问题**：如果让你优化这个项目，你觉得现在最大的短板在哪，你会从哪方面改进？

**口播版（第一人称，≥150 字）**：

我觉得最大的短板在数据通路：现在是"GPU 合成 → staging 纹理 Map 回读到 CPU → QImage → SwsContext 在 CPU 转 YUV → 再喂给编码器"，等于 GPU→CPU→GPU 白绕了两趟，Map 还是同步阻塞（等 GPU flush）。最该改的是硬件编码器直接吃 GPU 帧：FFmpeg 的 `h264_nvenc` 支持从 `AVHWFramesContext` 输入，可以把合成纹理不落 CPU、直接以 D3D11 纹理喂给 NVENC，1080p 下能省掉每帧几十毫秒的搬运，这是收益最大的一项。

其次是鲁棒性：编码器做 NVENC→QSV→libx264 的运行时优先级自动探测；RTMP 断线自动重连重推；音频设备用

 `IMMNotificationClient` 注册即插即用通知，替代现在的手动刷新。

再往后是工程化：给采集、编码、队列深度加性能指标埋点（帧率、Map 耗时、编码耗时、丢帧率），做 staging 双缓冲降低 Map 阻塞、UI 预览帧率节流，Win11 22H2+ 关掉 WGC 录制边框（`IsBorderRequired=false`），摄像头预热池改成按需懒加载避免无谓占带宽。总结一句：单点技术（采集、编码、推流）已经跑通，缺的是把"能跑"打磨成"稳定、低延迟、可观测"。

遇到困难怎么做的

拿学习 WGC 窗口采集 API 举例，当时 DXGI 采集带保护的窗口会得到黑帧，项目需要支持窗口捕获，于是学习 WGC 这套 WinRT 接口。先看微软官方示例，写最小 Demo 实现捕获单个窗口，拿到 GPU 纹理；Demo 跑通后集成进项目，处理帧池管理、版本兼容、资源释放等工程问题……