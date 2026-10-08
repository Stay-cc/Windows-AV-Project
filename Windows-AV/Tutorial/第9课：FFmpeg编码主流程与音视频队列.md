# 第9课：FFmpeg 编码主流程与音视频队列

> **目标**：深入理解项目如何把采集到的 `QImage` 和 PCM 音频编码成 MP4/RTMP，掌握 `FFmpegRecorder` 的线程模型、生产者-消费者队列、视频编码、音频重采样与混音、双路输出分发，以及软停止与淡出收尾的工程设计。  
> **对应文件**：`FFmpegRecorder.h`、`FFmpegRecorder.cpp`

---

## 1. FFmpegRecorder 在系统中的位置

前面几课我们已经知道：
- `DxGiCapturer` / `WgcCapturer` 负责视频帧生产
- `WasapiCapturer` 负责音频块生产
- `SystemController` 负责把这些数据路由过来

而 `FFmpegRecorder` 是整个项目的**编码与输出中心**。

它做的不是“采集”，而是：

1. 接收视频帧队列
2. 接收音频块队列
3. 做格式转换 / 重采样 / 混音
4. 编码成 H.264 + AAC
5. 同时写本地文件和/或 RTMP 推流

完整位置可以概括为：

```text
视频线程：DxGiCapturer  ─┐
                          ├─> FFmpegRecorder::pushVideoFrame / pushAudioData
音频线程：WasapiCapturer ─┘
                                   ↓
                            FFmpegRecorder::run()
                                   ↓
                        MP4 文件 / RTMP 直播流
```

所以它本质上是一个**消费者线程 + 编码枢纽**。

---

## 2. 为什么 FFmpegRecorder 继承 QThread？

头文件里定义：

```cpp
class FFmpegRecorder : public QThread
```

构造函数中还有一句：

```cpp
this->moveToThread(this);
```

这说明作者希望：
- `FFmpegRecorder` 自己就是一个线程对象
- 它的事件、槽函数也尽量在自己的线程上下文中处理

### 2.1 为什么不是普通 QObject + moveToThread？

因为这个模块的“核心逻辑”完全围绕 `run()` 循环展开：
- 初始化编码器
- 反复取队列
- 编码
- 写包
- 收尾

这种“线程本体 = 工作本体”的场景，用 `QThread` 继承是合理的。

### 2.2 `moveToThread(this)` 的意义

```cpp
QCoreApplication::processEvents();
```

在 `run()` 里这句非常关键。因为对象被移到自己的线程后，投递给它的事件/槽调用才会在编码线程被处理。

这也是第4课里提到的重点：

> `FFmpegRecorder` 不是单纯 while 循环，它还需要处理 Qt 事件。

否则某些投递到编码线程的调用可能永远得不到执行。

---

## 3. 头文件结构总览

先看最关键的数据结构。

### 3.1 视频帧结构

```cpp
struct VideoFrameData {
    QImage image;
    qint64 timestamp;
};
```

表示一帧视频：
- 图像内容
- 时间戳

### 3.2 音频块结构

```cpp
struct AudioChunk {
    QByteArray data;
    int sampleRate;
    int channels;
    qint64 timestamp;
};
```

表示一块音频：
- 原始 PCM 数据
- 输入采样率
- 输入通道数
- 时间戳（当前实现里未真正使用）

### 3.3 每个音频源的上下文

```cpp
struct SourceContext {
    SwrContext *swrCtx = nullptr;
    AVAudioFifo *fifo = nullptr;
    int inSampleRate = 0;
};
```

项目支持多个音频源并行输入，比如：
- 系统声音
- 麦克风

每一路源都要维护自己的：
- 重采样器 `SwrContext`
- 音频 FIFO
- 上次输入采样率信息

这为后面的混音做了基础。

---

## 4. 成员变量分层理解

### 4.1 录制状态

```cpp
std::atomic<bool> m_isRecording;
std::atomic<bool> m_softStopRequested;
qint64 m_firstFrameTimestamp = -1;
```

作用：
- `m_isRecording`：编码线程是否已正式开始接受数据
- `m_softStopRequested`：是否请求优雅停止
- `m_firstFrameTimestamp`：第一帧视频时间，用于音视频启动同步边界

### 4.2 视频编码资源

```cpp
AVCodecContext *m_vCtx = nullptr;
AVFrame *m_vFrame = nullptr;
SwsContext *m_swsCtx = nullptr;
```

职责：
- `m_vCtx`：视频编码器上下文
- `m_vFrame`：待送编码器的视频帧缓存
- `m_swsCtx`：像素格式/尺寸转换器（如 BGRA → YUV420P/NV12）

### 4.3 音频编码资源

```cpp
AVCodecContext *m_aCtx = nullptr;
AVFrame *m_aFrame = nullptr;
qint64 m_totalAudioSamples = 0;
```

职责：
- `m_aCtx`：AAC 编码器上下文
- `m_aFrame`：每次送给编码器的音频帧
- `m_totalAudioSamples`：累计已编码多少采样点，用来算音频 PTS

### 4.4 双路输出资源

```cpp
AVFormatContext *m_fmtCtxFile = nullptr;
AVStream *m_fileVStream = nullptr;
AVStream *m_fileAStream = nullptr;

AVFormatContext *m_fmtCtxStream = nullptr;
AVStream *m_streamVStream = nullptr;
AVStream *m_streamAStream = nullptr;
```

说明这个项目设计时就支持：
- 本地录制
- 直播推流
- 两者同时进行

### 4.5 队列

```cpp
QMutex m_videoMutex;
QQueue<VideoFrameData> m_videoQueue;
QMutex m_audioMutex;
QQueue<QPair<int, AudioChunk>> m_audioQueue;
```

这是标准的生产者-消费者模型：
- 采集线程加锁写入
- 编码线程加锁读取

---

## 5. startRecord()：开始录制前做了什么

```cpp
bool FFmpegRecorder::startRecord(const RecorderConfig& config)
```

这是编码流程的总入口。

### 5.1 防止重复启动

```cpp
if (isRunning()) return false;
```

如果线程还在跑，就直接拒绝。

### 5.2 重置直播时间戳跟踪

```cpp
m_lastStreamVideoPts = 0; m_lastStreamVideoDts = 0;
m_lastStreamAudioPts = 0; m_lastStreamAudioDts = 0;
```

后面直播写包时要强制保证时间戳单调，所以每次启动必须清零。

### 5.3 保存配置并修正输出尺寸

```cpp
m_config = config;
...
m_config.width -= (m_config.width % 2);
m_config.height -= (m_config.height % 2);
```

### 5.3.1 为什么宽高必须是偶数？

这是视频编码里的常见要求，特别是 YUV420 类格式：
- 色度采样通常要求偶数宽高
- 奇数尺寸容易导致编码失败或色度布局异常

所以作者在入口处就把分辨率修正掉，这是非常实用的工程防御。

### 5.4 至少要有一个输出目标

```cpp
if (!m_config.enableFileRecord && !m_config.enableStreaming) {
    return false;
}
```

如果既不录文件也不推流，那编码线程没有意义。

### 5.5 清空旧队列

```cpp
{ QMutexLocker l(&m_videoMutex); m_videoQueue.clear(); }
{ QMutexLocker l(&m_audioMutex); m_audioQueue.clear(); }
```

这是为了避免上一轮录制残留数据混入新录制。

### 5.6 启动线程

```cpp
m_softStopRequested = false;
m_firstFrameTimestamp = -1;
start();
```

注意：真正的编码初始化是在 `run()` 里完成，不是在这里。

---

## 6. stopRecord()：为什么叫“软停止”

```cpp
void FFmpegRecorder::stopRecord() {
    if (isRunning()) {
        m_softStopRequested = true;
    }
}
```

这里没有立即终止线程，没有 `terminate()`，也没有强行中断。

因为录制不能粗暴停：
- 队列里可能还有未编码的视频帧
- 音频需要做淡出
- 编码器内部可能还有缓存帧
- MP4/FLV 需要写 trailer

所以正确思路是：

> 先发出“该收尾了”的信号，再让编码循环自然完成尾部工作。

这就是 `softStop` 的含义。

---

## 7. pushVideoFrame()：视频生产者入口

```cpp
void FFmpegRecorder::pushVideoFrame(const QImage &img, qint64 timestamp)
```

### 7.1 非录制状态直接丢弃

```cpp
if (!m_isRecording || m_softStopRequested) return;
```

还没正式进入录制，或已经开始停止收尾，就不再接受新视频帧。

### 7.2 队列上限保护

```cpp
if (m_videoQueue.size() > 150) return;
```

这是很关键的实时系统设计：
- 编码赶不上采集时，不能无限堆积
- 150 帧约等于 30fps 下 5 秒缓冲
- 超过就丢新帧，避免内存爆炸

这和第4课里分析的一致。

### 7.3 为什么要深拷贝 QImage？

```cpp
if (img.format() != QImage::Format_RGBA8888)
    data.image = img.convertToFormat(QImage::Format_RGBA8888);
else
    data.image = img.copy();
```

原因：
- 视频线程中的原始图像内存不能假设在编码线程仍然有效
- 必须把图像数据独立出来

同时这里顺手统一为 `RGBA8888`，方便后面 `sws_scale()` 处理。

---

## 8. pushAudioData()：音频生产者入口

```cpp
void FFmpegRecorder::pushAudioData(int sourceId, const QByteArray &data,
                                   int sampleRate, int channels)
```

### 8.1 为什么音频要等第一帧视频之后才收？

```cpp
if (m_firstFrameTimestamp == -1) return;
```

这是本项目一个很聪明的同步策略。

含义是：
- 录制真正的“零点”以第一帧视频为准
- 在视频还没正式开始前，先到的音频不收

这样能避免：
- 音频先编码一大段静音/提前内容
- 音视频起点错位

这是一个简单但很有效的 A/V 启动同步方案。

### 8.2 音频块入队

```cpp
chunk.data = data;
chunk.sampleRate = sampleRate;
chunk.channels = channels;
chunk.timestamp = 0;
m_audioQueue.enqueue(qMakePair(sourceId, chunk));
```

其中 `sourceId` 用来标识来源，例如：
- `0`：系统音频
- `1`：麦克风

这样后面才能分别重采样、分别入 FIFO，再统一混音。

---

## 9. run()：编码线程主循环总览

`run()` 是整个录制系统最核心的函数。

它大致分成这几个阶段：

```text
1. 初始化视频编码器
2. 初始化音频编码器
3. 初始化文件输出 / 推流输出
4. 建立音频源上下文
5. 主循环：
   - 处理 Qt 事件
   - 取视频帧并编码
   - 检测 soft stop
   - 把音频块重采样后写入各自 FIFO
   - 根据视频进度混音并编码音频
6. flush 编码器
7. 写 trailer
8. 释放资源
```

---

## 10. 初始化阶段

### 10.1 初始化视频编码器

```cpp
if (!initVideoEncoder()) { ... }
```

### 10.2 初始化音频编码器

```cpp
if (!initAudioEncoder()) { ... }
```

### 10.3 初始化输出

```cpp
if (m_config.enableFileRecord) initFileOutput();
if (m_config.enableStreaming) initStreamOutput();
```

### 10.4 至少一个输出初始化成功

```cpp
if (!fileOk && !streamOk) { closeResources(); return; }
```

这很合理：
- 文件失败但推流成功，可以继续
- 推流失败但本地录制成功，也可以继续
- 两个都失败才终止

说明作者把“本地录制”和“直播”设计成了尽可能解耦的双出口。

---

## 11. ensureAudioSource()：多音源上下文延迟创建

```cpp
void FFmpegRecorder::ensureAudioSource(int id) {
    if (!m_audioSources.contains(id)) {
        SourceContext ctx;
        ctx.fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, 2, 1);
        m_audioSources.insert(id, ctx);
    }
}
```

每一路音频源都有自己的 FIFO。

### 11.1 为什么要分开 FIFO？

因为系统声音和麦克风：
- 到达时间不完全一致
- 原始采样率可能不同
- 不能简单把字节流拼在一起

必须先各自重采样、各自缓存，再按编码时间轴混到同一帧音频里。

这就是多路音频混音的基础。

---

## 12. 视频处理流程

### 12.1 从视频队列取帧

```cpp
VideoFrameData vFrameData;
bool hasVideo = false;
{
    QMutexLocker locker(&m_videoMutex);
    if (!m_videoQueue.isEmpty()) {
        vFrameData = m_videoQueue.dequeue();
        hasVideo = true;
    }
}
```

注意加锁范围很小：
- 只在取队列时加锁
- 取出后立刻解锁
- 真正编码在锁外进行

这是很好的并发习惯，否则生产者线程会被编码过程长期阻塞。

### 12.2 处理视频帧

```cpp
if (hasVideo) {
    emit frameProcessed();
    processVideo(vFrameData);
    lastVideoPts = vFrameData.timestamp;
}
```

这里 `lastVideoPts` 很重要，后面音频编码会参考它来判断“应该编码到哪个采样位置”。

---

## 13. processVideo()：QImage 到编码包

```cpp
void FFmpegRecorder::processVideo(const VideoFrameData &frameData)
```

### 13.1 第一帧决定音视频起点

```cpp
if (m_firstFrameTimestamp == -1) m_firstFrameTimestamp = frameData.timestamp;
```

一旦第一帧视频到来，音频就可以开始接收并对齐了。

### 13.2 创建 sws 上下文

```cpp
if (!m_swsCtx) {
    AVPixelFormat inputFmt = (frameData.image.format() == QImage::Format_RGBA8888)
        ? AV_PIX_FMT_RGBA : AV_PIX_FMT_BGRA;
    m_swsCtx = sws_getContext(...);
}
```

`sws_getContext()` 的作用是：
- 指定输入尺寸 / 像素格式
- 指定输出尺寸 / 像素格式
- 后续用 `sws_scale()` 做转换

这里是视频编码里非常经典的一步。

### 13.3 像素格式转换

```cpp
const uint8_t *srcSlice[] = { frameData.image.constBits() };
int srcStride[] = { (int)frameData.image.bytesPerLine() };
sws_scale(m_swsCtx, srcSlice, srcStride, 0, frameData.image.height(),
          m_vFrame->data, m_vFrame->linesize);
```

把 Qt 图像内存转换成编码器需要的格式，例如：
- `RGBA/BGRA` → `YUV420P`
- 或者硬件编码场景下转 `NV12`

### 13.4 计算视频 PTS

```cpp
qint64 diffMs = frameData.timestamp;
m_vFrame->pts = av_rescale_q(diffMs * 1000, {1, 1000000}, m_vCtx->time_base);
```

这里说明 `timestamp` 的单位是毫秒级语义，而作者先乘 `1000` 转成微秒，再 rescale 到视频编码器时间基。

本质就是：

> 用采集时间戳驱动编码时间戳。

### 13.5 编码视频帧

```cpp
if (avcodec_send_frame(m_vCtx, m_vFrame) >= 0) {
    AVPacket *pkt = av_packet_alloc();
    while (avcodec_receive_packet(m_vCtx, pkt) >= 0) {
        writePacket(pkt, true);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
}
```

这是 FFmpeg 新 API 的标准模式：
- `send_frame()` 把原始帧塞进去
- `receive_packet()` 循环取出编码包

注意：一次 `send_frame()` 可能对应 0、1 或多个输出包，所以必须循环 `receive_packet()`。

---

## 14. Soft Stop 与淡出逻辑

### 14.1 收到停止请求后，不马上退出

```cpp
if (m_softStopRequested && !isFadingOut) {
    isFadingOut = true;
    fadeOutStartSample = m_totalAudioSamples;
    qint64 samplesNeeded = ...;
    finalAudioSampleEnd = fadeOutStartSample + samplesNeeded;
}
```

此时系统进入“停止中”状态：
- 视频不再接收新帧
- 音频进入淡出阶段
- 仍继续处理队列和编码缓冲

### 14.2 退出条件

```cpp
if (isFadingOut && m_totalAudioSamples >= finalAudioSampleEnd
    && !hasVideo && m_videoQueue.isEmpty()) break;
```

退出前必须同时满足：
- 淡出音频已经补到目标点
- 当前没有正在处理的视频帧
- 视频队列已清空

这就是“优雅停止”的真正实现。

---

## 15. 音频块 → FIFO：重采样阶段

### 15.1 先把音频队列清空进 FIFO

```cpp
while(true) {
    QPair<int, AudioChunk> chunk;
    bool hasAudio = false;
    {
        QMutexLocker locker(&m_audioMutex);
        if (!m_audioQueue.isEmpty()) {
            chunk = m_audioQueue.dequeue();
            hasAudio = true;
        }
    }
    if (hasAudio) resampleToFifo(...);
    else break;
}
```

这里思路很清楚：
- 从跨线程队列里取原始块
- 尽快转入每路音频源自己的 FFmpeg FIFO
- 编码阶段只面向 FIFO，不再直接处理外部输入块

这能把“跨线程输入”和“编码时序”解耦。

---

## 16. resampleToFifo()：多源音频统一入口

```cpp
bool FFmpegRecorder::resampleToFifo(int sourceId, const QByteArray &data,
                                    int sampleRate, int channels)
```

### 16.1 如果采样率变化，重建重采样器

```cpp
if (!ctx.swrCtx || ctx.inSampleRate != sampleRate) {
    if (ctx.swrCtx) swr_free(&ctx.swrCtx);
    ...
    swr_alloc_set_opts2(&ctx.swrCtx, ...);
    swr_init(ctx.swrCtx);
    ctx.inSampleRate = sampleRate;
}
```

说明作者考虑到了输入设备采样率可能变化。每一路源都可以动态维护自己的 `SwrContext`。

### 16.2 输入与输出格式

当前输入来自 `WasapiCapturer`，已经统一为：

```cpp
AV_SAMPLE_FMT_FLT
```

而编码器 AAC 需要的是：

```cpp
AV_SAMPLE_FMT_FLTP
```

也就是**平面浮点**。

`swr_convert()` 负责把：
- 输入采样率 → 编码采样率
- 输入通道布局 → 输出双声道布局
- `FLT` → `FLTP`

全部统一掉。

### 16.3 写入 FIFO

```cpp
if (converted > 0) {
    if (av_audio_fifo_space(ctx.fifo) < converted)
        av_audio_fifo_realloc(...);
    av_audio_fifo_write(ctx.fifo, (void**)outData, converted);
}
```

这一步的意义是：

> 先把每一路源的重采样结果存起来，等到需要编码一帧音频时再统一拉取。

这比“来一包就立刻编码”要稳健得多，因为编码器要求固定 `frame_size` 的采样数。

---

## 17. 音频编码主逻辑：为什么要追视频进度？

编码循环里的关键代码：

```cpp
qint64 targetSamples = isFadingOut
    ? finalAudioSampleEnd
    : av_rescale_q(lastVideoPts, {1, 1000}, {1, m_aCtx->sample_rate});
while (m_totalAudioSamples < targetSamples) {
    ...
}
```

### 17.1 这是整套音视频同步的核心

它的意思是：
- 当前视频已经走到了 `lastVideoPts`
- 那音频就应该至少编码到对应的采样位置

因此音频不是盲目一直往前编码，而是：

> 以视频时间轴为参照，追赶到对应位置。

这是一种很实用的同步策略。

---

## 18. 混音：两路 FIFO 如何合成一帧音频

### 18.1 清空混音缓冲

```cpp
memset(mixBufferL, 0, frameSize * sizeof(float));
memset(mixBufferR, 0, frameSize * sizeof(float));
```

先把左右声道混音缓冲置零。

### 18.2 从每一路源 FIFO 中取一帧

```cpp
for (auto key : m_audioSources.keys()) {
    SourceContext &ctx = m_audioSources[key];
    if (!ctx.fifo) continue;
    if (av_audio_fifo_size(ctx.fifo) >= frameSize) {
        av_audio_fifo_read(ctx.fifo, tmpData, frameSize);
        for (int i = 0; i < frameSize; ++i) {
            mixBufferL[i] += tmpL[i];
            mixBufferR[i] += tmpR[i];
        }
    }
}
```

这就是最直接的混音方式：
- 把每一路源的样本叠加起来
- 左加左，右加右

如果某一路 FIFO 不够 `frameSize`，那一路本轮就等于静音。

这是一种简单且有效的实时混音策略。

---

## 19. 淡入 / 淡出：applyFadeEffect()

```cpp
void FFmpegRecorder::applyFadeEffect(float* bufferL, float* bufferR, int samples,
                                     qint64 totalEncodedSamples,
                                     bool isFadingOut, qint64 fadeOutStartSample)
```

### 19.1 淡入逻辑

```cpp
if (currentPos < fadeInSamples)
    gain = (float)currentPos / (float)fadeInSamples;
```

刚开始录制时，增益从 0 逐渐升到 1，避免“啪”一下硬切入。

### 19.2 淡出逻辑

```cpp
if (samplesSinceStop < fadeOutSamples)
    gain *= (1.0f - (float)samplesSinceStop / (float)fadeOutSamples);
else
    gain = 0.0f;
```

停止录制时，音量逐渐衰减到 0，而不是突然截断。

### 19.3 这是很有成品感的细节

很多简陋录制器只会“硬停”，结果就是：
- 开头有爆音
- 结尾有截断感

而这个项目加了淡入淡出，说明作者已经在往“可交付产品体验”靠近。

---

## 20. 限幅：为什么要 clamp

```cpp
mixBufferL[i] = std::clamp(mixBufferL[i], -1.0f, 1.0f);
mixBufferR[i] = std::clamp(mixBufferR[i], -1.0f, 1.0f);
```

因为两路音频相加后可能超过 `[-1, 1]`，如果不限制：
- 会发生削波失真
- 后续编码器输入也不规范

所以混音完成后必须做一次限幅。

---

## 21. 把混音结果送入 AAC 编码器

```cpp
av_frame_make_writable(m_aFrame);
memcpy(m_aFrame->data[0], mixBufferL, frameSize * sizeof(float));
memcpy(m_aFrame->data[1], mixBufferR, frameSize * sizeof(float));
m_aFrame->pts = av_rescale_q(m_totalAudioSamples,
                             {1, m_aCtx->sample_rate},
                             m_aCtx->time_base);
m_totalAudioSamples += frameSize;
```

### 21.1 为什么 `data[0]`、`data[1]` 分开拷贝？

因为编码器使用的是：

```cpp
AV_SAMPLE_FMT_FLTP
```

也就是**平面格式**：
- 左声道一个平面
- 右声道一个平面

所以不能像交错格式那样一块 memcpy 全部通道。

### 21.2 音频 PTS 为什么按采样数计算？

音频最自然的时间单位就是样本数。

所以：
- 当前已编码 `m_totalAudioSamples`
- 按 `sample_rate` 转成时间基
- 得到本帧音频 PTS

这比用毫秒整数去凑更精确。

### 21.3 编码流程和视频一样

```cpp
avcodec_send_frame(m_aCtx, m_aFrame)
avcodec_receive_packet(m_aCtx, pkt)
```

还是 send/receive 模式。

---

## 22. initVideoEncoder()：视频编码器初始化

### 22.1 允许按名字选择编码器

```cpp
const AVCodec *codec = avcodec_find_encoder_by_name(codecName.toStdString().c_str());
```

这意味着配置里可以指定：
- `h264_nvenc`
- `libx264`
- 其他 FFmpeg 可用编码器

### 22.2 参数设置

```cpp
m_vCtx->width = m_config.width;
m_vCtx->height = m_config.height;
m_vCtx->time_base = {1, 1000000};
m_vCtx->framerate = {m_config.fps, 1};
m_vCtx->gop_size = m_config.fps * 2;
m_vCtx->max_b_frames = 0;
m_vCtx->bit_rate = m_config.videoBitrate;
```

这里可以看出作者偏向低延迟实时场景：
- `max_b_frames = 0` 避免 B 帧带来的重排延迟
- GOP 约 2 秒

### 22.3 全局头部标志非常关键

```cpp
m_vCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
```

代码注释已经说得很明确：
- MP4 / FLV 都需要 SPS/PPS 等全局头信息
- 尤其 RTMP 场景更依赖这类配置

这就是所谓“容器需要 codec extradata”。

### 22.4 像素格式按编码器选择

```cpp
if (codecName.contains("nvenc") || codecName.contains("qsv"))
    m_vCtx->pix_fmt = AV_PIX_FMT_NV12;
else
    m_vCtx->pix_fmt = AV_PIX_FMT_YUV420P;
```

说明作者对硬件编码器特性是有针对性处理的：
- 硬件编码器常偏好 `NV12`
- 软件 `libx264` 更常用 `YUV420P`

### 22.5 自动降级

```cpp
if (m_config.encoderName != "libx264" && tryOpenCodec("libx264")) {
    m_config.encoderName="libx264";
}
```

如果用户指定的硬件编码器打不开，就自动退回 `libx264`。

这很符合桌面软件的容错思路：
- 能录下来最重要
- 不要因为显卡编码器不可用就整个功能失效

---

## 23. initAudioEncoder()：AAC 编码器初始化

```cpp
const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
```

项目音频统一编码为 AAC。

### 23.1 核心参数

```cpp
m_aCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
m_aCtx->bit_rate = m_config.audioBitrate;
m_aCtx->sample_rate = m_config.audioSampleRate;
```

### 23.2 通道布局固定为双声道

```cpp
AVChannelLayout layout;
av_channel_layout_default(&layout, 2);
av_channel_layout_copy(&m_aCtx->ch_layout, &layout);
```

说明当前项目最终输出统一为立体声。

### 23.3 音频也必须带全局头部

```cpp
m_aCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
```

和视频一样，AAC 在 MP4/FLV 中也需要这类 codec config 信息。

### 23.4 创建音频帧

```cpp
m_aFrame->nb_samples = m_aCtx->frame_size;
m_aFrame->format = m_aCtx->sample_fmt;
m_aFrame->sample_rate = m_aCtx->sample_rate;
```

AAC 编码器通常要求固定帧大小，所以项目后面都是按 `frame_size` 为单位取音频做编码。

---

## 24. initFileOutput()：本地录制输出

```cpp
avformat_alloc_output_context2(&m_fmtCtxFile, nullptr, nullptr,
                               m_config.savePath.toStdString().c_str());
```

FFmpeg 会根据文件扩展名推断封装格式，例如 `.mp4`。

后续：
- 创建视频流 `m_fileVStream`
- 创建音频流 `m_fileAStream`
- 从编码器上下文复制 codecpar
- `avio_open()` 打开输出文件
- `avformat_write_header()` 写文件头

本地录制相对“宽容”，时间戳容错通常比直播高。

---

## 25. initStreamOutput()：RTMP 推流输出

### 25.1 组装完整 RTMP 地址

```cpp
QString fullUrl = m_config.rtmpUrl;
if (!m_config.streamKey.isEmpty()) {
    if (!fullUrl.endsWith("/")) fullUrl += "/";
    fullUrl += m_config.streamKey;
}
```

### 25.2 强制使用 FLV 容器

```cpp
avformat_alloc_output_context2(&m_fmtCtxStream, nullptr, "flv", fullUrl.toStdString().c_str());
```

RTMP 常用 FLV 封装，这是标准做法。

### 25.3 直播参数调优

```cpp
av_dict_set(&options, "rtmp_transport", "tcp", 0);
av_dict_set(&options, "rtmp_buffer", "1000", 0);
av_dict_set(&options, "buffer_size", "4096000", 0);
av_dict_set(&options, "rw_timeout", "5000000", 0);
av_dict_set(&options, "tcp_nodelay", "1", 0);
```

这些选项体现了作者对 RTMP 实战问题的处理：
- 强制 TCP：更稳定
- 缓冲适中：兼顾流畅和延迟
- 禁用 Nagle：降低发送延迟
- 设置超时：避免长时间卡死

这不是“能跑就行”的代码，而是带有实战调参痕迹的代码。

---

## 26. writePacket()：为什么要做双路分发

```cpp
void FFmpegRecorder::writePacket(AVPacket* pkt, bool isVideo)
```

这就是整套“双输出”架构的关键。

输入是一个已经编码好的包，函数内部会：
- 克隆一份写本地文件
- 再克隆一份写直播流

### 26.1 为什么要 clone 包？

因为：
- 每个输出流的时间基不同
- `av_packet_rescale_ts()` 会修改包里的时间戳
- 一个包不能直接重复拿去写两个目标

所以要各自 clone，再分别调整。

---

## 27. 文件输出 vs 直播输出的差异

### 27.1 文件输出比较宽容

```cpp
av_packet_rescale_ts(filePkt, srcTb, outStream->time_base);
av_interleaved_write_frame(m_fmtCtxFile, filePkt);
```

本地文件通常能接受较正常的时间戳即可。

### 27.2 直播输出必须严格单调

```cpp
if (streamPkt->dts < *lastDts) {
    streamPkt->dts = *lastDts + 1;
}
if (streamPkt->pts < streamPkt->dts) {
    streamPkt->pts = streamPkt->dts;
}
```

这是本项目直播稳定性的一个“核心修复”。

RTMP 服务器通常非常讨厌：
- DTS 回退
- PTS < DTS
- 时间戳不单调

这些情况很容易直接导致断流。

所以作者在写直播包前，强制纠正时间戳单调性。

这是非常典型的工程性补强。

---

## 28. 推流失败为什么不影响本地录制？

```cpp
if (ret == -10053 || ret == AVERROR(EPIPE)) {
    ...
    avformat_free_context(m_fmtCtxStream);
    m_fmtCtxStream = nullptr;
}
```

如果直播连接断了：
- 只关闭直播输出上下文
- 本地录制相关资源不动

因此后续：
- 本地录制仍可继续
- 推流路径自动熔断

这说明作者明确把“录制”和“直播”当成两个独立故障域。

这是一个非常好的产品级设计。

---

## 29. flushEncoders()：为什么收尾时一定要 flush

```cpp
avcodec_send_frame(m_vCtx, nullptr);
while(avcodec_receive_packet(m_vCtx, pkt) >= 0) { ... }

avcodec_send_frame(m_aCtx, nullptr);
while(avcodec_receive_packet(m_aCtx, pkt) >= 0) { ... }
```

向编码器发送 `nullptr` 帧，表示：

> 没有更多原始数据了，把内部缓存的包都吐出来。

如果不 flush：
- 尾部几帧可能丢失
- 文件时长不完整
- 直播最后一段可能缺失

---

## 30. 为什么一定要写 trailer

```cpp
if (m_fmtCtxFile) av_write_trailer(m_fmtCtxFile);
if (m_fmtCtxStream) av_write_trailer(m_fmtCtxStream);
```

尤其对 MP4 来说，trailer 非常重要：
- 它会写入索引和尾部元数据
- 不写的话，文件往往不可播放或结构不完整

这也是第4课里专门强调过的。

---

## 31. closeResources()：资源释放顺序

`closeResources()` 负责统一释放：
- `AVFrame`
- `AVCodecContext`
- `AVFormatContext`
- `SwsContext`
- 每个音频源的 `SwrContext` / `AVAudioFifo`

这说明作者没有把释放逻辑散落在多个地方，而是集中封装成一个统一收尾函数。

这种设计有两个好处：
1. 出错路径也能复用同一套清理逻辑
2. 更不容易遗漏 FFmpeg 资源

---

## 32. 这份编码器实现的几个工程亮点

### 32.1 用第一帧视频作为音视频起点

这是简单而有效的同步策略。

### 32.2 双输出分发而不是双编码

项目只编码一次，然后写两个目标：
- 节省 CPU/GPU
- 避免两套编码器导致额外同步复杂度

### 32.3 直播路径做了比文件路径更严格的时间戳处理

说明作者知道网络流媒体对时间戳更敏感。

### 32.4 多音源先各自进 FIFO 再混音

这让：
- 麦克风
- 系统音频

能在不同采样率、不同到达节奏下稳定融合。

### 32.5 软停止 + 淡出 + flush + trailer 是完整闭环

这不是“能录到”而已，而是完整考虑了：
- 用户点击停止后的体验
- 文件可播放性
- 直播断线容错

---

## 33. 本课作业

1. **流程图练习**：画出 `pushVideoFrame / pushAudioData → 队列 → run() → 编码器 → writePacket() → 文件/推流` 的完整数据流图。

2. **同步思考题**：为什么 `pushAudioData()` 在 `m_firstFrameTimestamp == -1` 时直接丢弃音频？如果去掉这条判断，音视频可能出现什么问题？

3. **混音题**：项目为什么不直接把麦克风和系统声音拼接到同一个队列里统一处理，而是为每个 `sourceId` 分配独立的 `SwrContext + FIFO`？

4. **直播稳定性题**：解释 `writePacket()` 中“强制修正 DTS/PTS 单调性”的必要性。为什么本地 MP4 对这个问题更宽容，而 RTMP 更敏感？

---

*上一课：[第8课：音频采集与WASAPI基础](./第8课：音频采集与WASAPI基础.md)*  
*下一课：[第10课：本地录制与RTMP推流输出](./第10课：本地录制与RTMP推流输出.md)*
