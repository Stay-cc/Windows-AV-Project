# 第10课：FFmpeg 混流与时间戳同步

> **目标**：深入理解项目如何把已经编码好的 H.264/AAC 数据正确封装到 MP4/FLV，掌握 `FFmpegRecorder` 中的时间戳体系、音视频同步策略、双流交织写入、直播单调性修正，以及 `flush + trailer` 对最终文件/推流稳定性的决定性作用。  
> **对应文件**：`FFmpegRecorder.cpp`

---

## 1. 第10课和第9课的关系

第9课我们重点讲了：
- 视频帧和音频块如何进入编码线程
- `QImage` 如何变成 H.264 包
- PCM 如何变成 AAC 包
- 多音源如何进 FIFO、混音、淡入淡出

但“编码出包”还不是整个链路的终点。

真正能生成：
- 可播放的 MP4
- 可被 RTMP 服务端接受的直播流

还需要解决两个更难的问题：

1. **这些包该按什么时间顺序写出去？**
2. **视频包和音频包的时间戳如何保持一致？**

所以第10课关注的是：

> **编码后的包如何被组织成一条正确的时间轴，并安全写入容器/网络输出。**

可以把主链路补完整为：

```text
采集线程产生原始数据
    ↓
FFmpegRecorder 编码线程
    ├─ processVideo() 生成 H.264 AVPacket
    └─ 音频混音后生成 AAC AVPacket
    ↓
writePacket()
    ├─ 写入 MP4 文件
    └─ 写入 FLV/RTMP 直播流
    ↓
flushEncoders()
    ↓
av_write_trailer()
```

---

## 2. 为什么“编码成功”不等于“录制成功”

很多初学者会以为：
- 只要 `avcodec_send_frame()` 成功
- 只要 `avcodec_receive_packet()` 能拿到包
- 录制就已经完成了

其实并不是。

因为编码器输出的 `AVPacket` 只是“裸编码包”或“压缩帧”，还没有回答这些问题：

- 这个包属于哪一路流？视频还是音频？
- 它在容器里的时间基是什么？
- 应该先写视频包还是先写音频包？
- MP4 索引什么时候写？
- RTMP 的时间戳如果回退怎么办？

所以真正的录制系统至少包含三层：

```text
第1层：采集（桌面、摄像头、麦克风、系统音频）
第2层：编码（H.264 / AAC）
第3层：封装与输出（MP4 / FLV / RTMP）
```

本课讲的就是第三层的核心工程问题。

---

## 3. 时间戳体系：为什么这是音视频系统的核心

在普通业务开发里，时间通常是：
- 毫秒整数
- `QDateTime`
- `std::chrono`

但在 FFmpeg 里，时间不是简单的“一个 long long 毫秒值”，而是：

> **一个数值 + 一个时间基（time_base）**

也就是说，**同样的 pts=1000，在不同 time_base 下，真实时间完全不同。**

### 3.1 time_base 是什么

例如：

```cpp
m_vCtx->time_base = {1, 1000000};
m_aCtx->time_base = {1, m_aCtx->sample_rate};
```

含义分别是：

- 视频编码器时间基：1 个单位 = 1 微秒
- 音频编码器时间基：1 个单位 = 1 / sample_rate 秒

例如采样率 48000：
- 音频 pts 每增加 48000，表示前进 1 秒

### 3.2 PTS 和 DTS 是什么

- **PTS（Presentation Timestamp）**：显示/播放时间戳
- **DTS（Decoding Timestamp）**：解码时间戳

对当前项目来说：
- 视频设置了 `max_b_frames = 0`
- 所以不会引入 B 帧重排
- 因此视频 PTS/DTS 关系相对简单

但到了 RTMP/FLV 场景，服务器仍然非常在意：
- DTS 必须单调递增
- PTS 不能小于 DTS

这就是 `writePacket()` 中那段“强制修正时间戳”的背景。

---

## 4. 项目里有哪些时间基

在 `FFmpegRecorder.cpp` 里，至少有三套需要区分的时间坐标：

### 4.1 采集侧时间语义

视频帧进入时：

```cpp
void FFmpegRecorder::pushVideoFrame(const QImage &img, qint64 timestamp)
```

这里的 `timestamp` 是项目上游传入的采集时间，语义是**毫秒**。

后面在 `processVideo()` 中：

```cpp
qint64 diffMs = frameData.timestamp;
m_vFrame->pts = av_rescale_q(diffMs * 1000, {1, 1000000}, m_vCtx->time_base);
```

作者先把毫秒乘以 1000 转成微秒，再按视频编码器时间基缩放。

### 4.2 音频侧样本计数时间

音频不是按毫秒直接推进，而是按样本数推进：

```cpp
m_aFrame->pts = av_rescale_q(m_totalAudioSamples,
                             {1, m_aCtx->sample_rate},
                             m_aCtx->time_base);
```

这里说明：

> 对音频而言，最自然的“时间”单位其实是样本数，而不是毫秒。

### 4.3 输出流时间基

写包前还要再转换一次：

```cpp
av_packet_rescale_ts(filePkt, srcTb, outStream->time_base);
```

也就是说，一个包会经历：

```text
采集时间语义
   ↓
编码器时间基
   ↓
输出流时间基
```

这是 FFmpeg 很多代码最容易看晕的地方。

---

## 5. `AVFormatContext`：容器/输出的总控制器

第9课我们已经见过：

```cpp
AVFormatContext *m_fmtCtxFile = nullptr;
AVFormatContext *m_fmtCtxStream = nullptr;
```

这两个对象分别表示：

- `m_fmtCtxFile`：本地文件输出上下文
- `m_fmtCtxStream`：直播输出上下文

### 5.1 它们分别对应什么

本地录制：

```cpp
avformat_alloc_output_context2(&m_fmtCtxFile, nullptr, nullptr,
                               m_config.savePath.toStdString().c_str());
```

直播推流：

```cpp
avformat_alloc_output_context2(&m_fmtCtxStream, nullptr, "flv",
                               fullUrl.toStdString().c_str());
```

说明：
- 文件输出通常根据扩展名自动判断容器
- RTMP 输出明确指定 `flv`

### 5.2 `AVFormatContext` 负责什么

它并不做编码，而是负责：
- 管理容器格式
- 持有各路 `AVStream`
- 写 header
- 写交织数据包
- 写 trailer
- 管理 I/O（文件句柄或网络连接）

所以可以把它理解成：

> **编码后的包，最终都要交给 `AVFormatContext` 才能变成真正的“文件”或“直播流”。**

---

## 6. `AVStream`：每一路流的容器内描述

项目中有四个流指针：

```cpp
AVStream *m_fileVStream = nullptr;
AVStream *m_fileAStream = nullptr;
AVStream *m_streamVStream = nullptr;
AVStream *m_streamAStream = nullptr;
```

它们表示：
- 文件输出的视频流
- 文件输出的音频流
- 直播输出的视频流
- 直播输出的音频流

### 6.1 为什么文件和直播不能共用 `AVStream`

因为它们分别属于两个不同的 `AVFormatContext`。

也就是说：
- MP4 容器里有自己的一套 stream 元数据
- FLV 容器里也有自己的一套 stream 元数据

即使它们承载的是同一份编码结果，也必须在各自容器中单独建流。

### 6.2 流参数从哪里来

项目通过：

```cpp
avcodec_parameters_from_context(m_fileVStream->codecpar, m_vCtx);
avcodec_parameters_from_context(m_fileAStream->codecpar, m_aCtx);
```

把编码器配置复制到流描述里。

这一步非常重要，因为容器需要知道：
- 编码类型（H.264 / AAC）
- 分辨率
- 采样率
- 通道布局
- extradata（例如 SPS/PPS、AAC config）

这也是第9课里强调 `AV_CODEC_FLAG_GLOBAL_HEADER` 的原因。

---

## 7. 输出初始化：为什么 header 一定要先写

### 7.1 文件输出初始化流程

`initFileOutput()` 中的关键流程：

```cpp
avformat_alloc_output_context2(...)
avformat_new_stream(...)
avcodec_parameters_from_context(...)
avio_open(...)
avformat_write_header(...)
```

### 7.2 直播输出初始化流程

`initStreamOutput()` 中同样也是：

```cpp
avformat_alloc_output_context2(..., "flv", ...)
avformat_new_stream(...)
avcodec_parameters_from_context(...)
avio_open2(...)
avformat_write_header(...)
```

### 7.3 为什么 header 不能省略

因为容器在真正写入媒体包前，必须先把“这是什么流”告诉接收方。

例如：
- MP4 需要知道有几路流以及它们的 codec 配置
- RTMP/FLV 需要先发送 AVC/AAC 的配置信息，服务端才能正确解析后续包

如果没写 header：
- 文件可能不可播放
- 服务端可能直接拒绝流
- 即使勉强写进去，也容易出现无法解析的现象

所以：

> `avformat_write_header()` 不是可选项，而是整个封装流程真正开始的标志。

---

## 8. 视频 PTS 是怎么来的

看 `processVideo()`：

```cpp
qint64 diffMs = frameData.timestamp;
m_vFrame->pts = av_rescale_q(diffMs * 1000, {1, 1000000}, m_vCtx->time_base);
```

### 8.1 这行代码的实际意义

可以拆成三步理解：

1. `frameData.timestamp` 是毫秒语义
2. `* 1000` 转成微秒
3. `av_rescale_q(..., {1,1000000}, m_vCtx->time_base)` 转成视频编码器时间基

而当前视频编码器时间基恰好也是：

```cpp
m_vCtx->time_base = {1, 1000000};
```

所以现在基本相当于“直接用微秒作视频时间”。

### 8.2 为什么这样设计很方便

因为视频采集天然更适合由“真实墙钟时间”驱动。

一帧什么时候被采到，就给它什么时间戳，这样：
- 帧率轻微波动也能自然表达
- 丢帧不会破坏整体时间语义
- 后续音频只需要去追赶视频时间轴即可

这就是项目的同步基础。

---

## 9. 音频 PTS 是怎么来的

音频这边不使用外部毫秒戳，而是自己累计：

```cpp
m_aFrame->pts = av_rescale_q(m_totalAudioSamples,
                             {1, m_aCtx->sample_rate},
                             m_aCtx->time_base);
m_totalAudioSamples += frameSize;
```

### 9.1 为什么音频适合用样本数驱动

因为音频的本质就是连续采样点。

例如 48kHz：
- 每秒 48000 个样本
- 已经编码了 96000 个样本，就说明音频时间走了 2 秒

这是比“每帧都自己算毫秒”更精确、更自然的做法。

### 9.2 这和视频有什么不同

视频：
- 更像一个个离散图像
- 用采集时刻做 PTS 很自然

音频：
- 是连续样本流
- 用累计样本数做 PTS 更自然

这也是很多音视频系统的典型分工：
- 视频靠采样时刻
- 音频靠样本计数

---

## 10. 项目的音视频同步策略：音频追视频

第9课提过，这里再从“同步”角度完整看一次：

```cpp
qint64 targetSamples = isFadingOut
    ? finalAudioSampleEnd
    : av_rescale_q(lastVideoPts, {1, 1000}, {1, m_aCtx->sample_rate});

while (m_totalAudioSamples < targetSamples) {
    ...
}
```

### 10.1 这段代码是什么意思

它的含义是：

- `lastVideoPts` 表示视频目前已经推进到哪里
- 把这个视频时间点换算成“音频应该到达多少样本”
- 如果当前音频还没跟上，就继续编码音频

也就是：

> **不是视频追音频，而是音频始终追赶视频当前进度。**

### 10.2 为什么这个策略适合本项目

因为本项目的启动边界已经被固定为：

```cpp
if (m_firstFrameTimestamp == -1) return;
```

也就是“音频等第一帧视频到来后再开始”。

于是整个时间轴天然以视频启动为零点，音频只需不断补到视频所在位置。

### 10.3 这种做法解决了什么问题

它避免了：
- 音频先启动一大段
- 视频第一帧来得晚导致开头不同步
- 因采集节奏不一致产生的明显 A/V 起点偏移

这是一种非常实用、也很符合桌面录屏场景的工程方案。

---

## 11. 为什么不是“拿到音频就立刻写包”

很多人一开始会想：
- 麦克风来一块 PCM 就编码一块
- 视频来一帧就编码一帧
- 然后谁先来就先写谁

但这样做在真实系统里很容易出问题。

### 11.1 原因一：音频编码器要求固定帧大小

AAC 编码器通常不是“来多少采样就吃多少”，而是需要固定 `frame_size`。

项目里就是按：

```cpp
int frameSize = m_aCtx->frame_size;
```

来组织音频帧。

### 11.2 原因二：多音源到达节奏不一致

系统声音和麦克风：
- 到包时间不同
- 采样率可能不同
- 包大小可能不同

所以必须先：
- 各自重采样
- 各自进 FIFO
- 到编码时再按统一时间轴取样混音

### 11.3 原因三：封装层需要稳定时间轴

容器和 RTMP 服务端最怕的是：
- 包一会儿快，一会儿慢
- 时间戳乱跳
- 流顺序混乱

所以项目采用的是：

```text
原始输入块 → FIFO
        ↓
按目标时间轴决定何时编码
        ↓
编码出包 → 按容器规则写入
```

这才是更稳的系统设计。

---

## 12. `writePacket()`：混流和分发的真正核心

本课最重要的函数就是：

```cpp
void FFmpegRecorder::writePacket(AVPacket* pkt, bool isVideo)
```

它解决了两个关键问题：

1. **一个编码包如何进入文件输出**
2. **同一个编码包如何同时进入直播输出**

### 12.1 输入是什么

传进来的 `pkt` 已经是编码器吐出的压缩包：
- `isVideo = true` 表示 H.264 包
- `isVideo = false` 表示 AAC 包

### 12.2 这个函数为什么很关键

因为它把第9课的“编码器世界”和本课的“容器/输出世界”连接起来了。

你可以把它理解成：

> **所有音视频数据最终离开 FFmpegRecorder 的唯一出口。**

---

## 13. 为什么要 `av_packet_clone()`

在 `writePacket()` 里，作者先这样做：

```cpp
AVPacket* filePkt = av_packet_clone(pkt);
AVPacket* streamPkt = av_packet_clone(pkt);
```

### 13.1 为什么不能同一个包写两次

因为后面会调用：

```cpp
av_packet_rescale_ts(...)
```

这个函数会**原地修改时间戳**。

而文件输出和直播输出：
- 属于不同的 `AVStream`
- 各自可能有不同的 `time_base`
- 后续直播路径还会做额外的 PTS/DTS 修正

如果复用同一个 `pkt`：
- 先给文件路径改一遍时间戳
- 再给直播路径改一遍
- 就会互相污染

所以必须 clone。

### 13.2 这背后的架构思想

项目采用的是：

> **一次编码，多路分发**

而不是：
- 文件输出用一套编码器
- 直播输出再开一套编码器

这样做的好处非常大：
- 节省 CPU/GPU
- 避免双编码导致的额外延迟
- 避免两套编码器各自产生不同时间轴

---

## 14. 写文件路径：为什么相对宽容

文件输出部分：

```cpp
if (m_fmtCtxFile) {
    AVPacket* filePkt = av_packet_clone(pkt);
    if (filePkt) {
        AVStream* outStream = isVideo ? m_fileVStream : m_fileAStream;
        av_packet_rescale_ts(filePkt, srcTb, outStream->time_base);
        filePkt->stream_index = outStream->index;
        av_interleaved_write_frame(m_fmtCtxFile, filePkt);
    }
}
```

### 14.1 关键步骤

1. clone 包
2. 根据目标流时间基做 `rescale`
3. 指定 `stream_index`
4. 交给 `av_interleaved_write_frame()`

### 14.2 为什么文件路径通常更宽容

因为 MP4 是本地文件：
- 不需要实时被网络服务端校验
- muxer 通常有更强的缓冲和整理能力
- 只要整体时间戳大体合理，通常都能正常播放

当然这不意味着文件路径可以完全乱写，只是它对短暂的不完美更能容忍。

---

## 15. `av_interleaved_write_frame()` 为什么比直接写更重要

项目用的是：

```cpp
av_interleaved_write_frame(...)
```

而不是简单的 `av_write_frame()`。

### 15.1 什么叫 interleaved

“交织写入”就是指：
- 容器中视频包和音频包不是两坨分开写
- 而是按时间轴交替组织

例如：

```text
V0 A0 A1 V1 A2 A3 V2 ...
```

而不是：

```text
V0 V1 V2 ... A0 A1 A2 ...
```

### 15.2 为什么交织很重要

因为播放器和流媒体系统都希望：
- 按时间顺序拿到多路流
- 不用等很久才能等到另一种媒体类型

尤其是直播场景，如果交织不好：
- 播放端可能卡顿
- 音画不同步更明显
- 某些服务端会更难处理

所以 `av_interleaved_write_frame()` 是更符合容器/播放预期的做法。

---

## 16. 直播输出为什么更脆弱

直播路径里，作者专门写了大量“更严格”的逻辑：

```cpp
if (streamPkt->dts < *lastDts) {
    streamPkt->dts = *lastDts + 1;
}
if (streamPkt->pts < streamPkt->dts) {
    streamPkt->pts = streamPkt->dts;
}
```

这说明：

> **网络直播输出对时间戳错误比文件输出更敏感。**

### 16.1 为什么 RTMP 更敏感

因为 RTMP 服务器是“边收边处理”的：
- 它不能像本地文件那样事后回看整段数据再修补
- 也不会宽容地等待你未来再把时间轴解释清楚

一旦看到：
- DTS 回退
- PTS < DTS
- 时间戳不单调

很多服务器就会：
- 直接报错
- 断开连接
- 或者导致下游播放器异常

### 16.2 本项目的经验痕迹

代码注释里已经说明：

```cpp
// RTMP 服务器非常讨厌时间戳回退，这会导致立即断开 (-10053)
```

这不是教科书里的抽象结论，而是作者在真实推流场景里踩出来的工程经验。

---

## 17. 直播时间戳单调性修正到底在修什么

项目为直播分别记录了：

```cpp
m_lastStreamVideoPts
m_lastStreamVideoDts
m_lastStreamAudioPts
m_lastStreamAudioDts
```

### 17.1 为什么音视频要分开记录

因为：
- 视频流有自己的一条时间线
- 音频流也有自己的一条时间线
- 它们分别在各自 stream 内保持单调即可

### 17.2 修正规则

#### 规则1：DTS 不能倒退

```cpp
if (streamPkt->dts < *lastDts) {
    streamPkt->dts = *lastDts + 1;
}
```

含义：
- 如果当前包的解码时间比上一包还早
- 那就强行把它推到上一包后面

#### 规则2：PTS 不能早于 DTS

```cpp
if (streamPkt->pts < streamPkt->dts) {
    streamPkt->pts = streamPkt->dts;
}
```

含义：
- 展示时间不应该比解码时间更早
- 否则对很多 muxer / server 都是不合法的

### 17.3 这是不是“理论最优解”？

不一定。

它更像是：
- 一个务实的稳定性补丁
- 保证直播链路不中断
- 优先满足服务端对单调性的刚性要求

这就是典型的工程取舍：
- 先活下来
- 再谈最理想时间模型

---

## 18. `stream_index` 为什么必须设置

在写包前，项目会这样做：

```cpp
filePkt->stream_index = outStream->index;
streamPkt->stream_index = outStream->index;
```

### 18.1 这是什么意思

一个 `AVFormatContext` 里可以有多路流：
- 视频流 index 可能是 0
- 音频流 index 可能是 1

封装器必须知道：
- 这个包属于哪一路流
- 才能把它写到对应 track / stream 上

### 18.2 如果不设会怎样

如果不正确设置：
- muxer 无法判断包的归属
- 写出的容器结构可能错误
- 播放器可能把音频当视频，或直接报错

所以：

> `stream_index` 是“这个包属于谁”的身份标签。

---

## 19. 文件输出和直播输出为什么是两个故障域

看 `writePacket()` 的直播错误处理：

```cpp
if (ret == -10053 || ret == AVERROR(EPIPE)) {
    if (!(m_fmtCtxStream->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&m_fmtCtxStream->pb);
    }
    avformat_free_context(m_fmtCtxStream);
    m_fmtCtxStream = nullptr;
}
```

### 19.1 这段代码说明了什么

如果直播断了：
- 只把直播输出关闭
- 不影响文件输出
- 之后继续录本地 MP4

### 19.2 为什么这是很好的设计

因为在产品层面：
- 用户直播失败了，至少还希望本地录像保住
- 网络是高故障概率路径
- 本地文件和网络推流不应该互相拖死

所以项目明确把二者分成了两个故障域：

```text
直播挂了 ≠ 本地录制也必须挂
```

这比“任一路失败就全部停止”更符合真实用户预期。

---

## 20. 为什么启动时要重置直播时间戳跟踪器

`startRecord()` 中：

```cpp
m_lastStreamVideoPts = 0; m_lastStreamVideoDts = 0;
m_lastStreamAudioPts = 0; m_lastStreamAudioDts = 0;
```

### 20.1 这不是小细节，而是必须的

因为如果上一次录制/推流结束后不清零：
- 新的一次输出会继承旧的“最后时间戳”
- 导致一开播时间戳就异常偏大
- 或者被错误地判定为回退/不连续

所以每次启动都必须重置时间戳状态。

### 20.2 这也说明什么

说明 `writePacket()` 的单调修正逻辑依赖“会话内状态”。

因此：
- 它不是纯函数
- 而是一个和当前推流会话绑定的状态机逻辑

---

## 21. `flushEncoders()`：为什么收尾时还会继续出包

```cpp
avcodec_send_frame(m_vCtx, nullptr);
while(avcodec_receive_packet(m_vCtx, pkt) >= 0) { ... }

avcodec_send_frame(m_aCtx, nullptr);
while(avcodec_receive_packet(m_aCtx, pkt) >= 0) { ... }
```

### 21.1 向编码器发送 `nullptr` 的含义

这不是空操作，而是告诉编码器：

> 原始输入已经结束，请把内部缓冲里还没吐出的包全部吐出来。

### 21.2 为什么编码器内部会有缓存

因为编码器不是“你喂一帧就立刻必出一包”的简单函数，它内部可能：
- 有 lookahead
- 有重排缓冲
- 有压缩状态积累
- 有未输出的尾包

即使当前项目用了低延迟参数，flush 仍然是必须的。

### 21.3 如果不 flush 会怎样

常见后果：
- 结尾少几帧视频
- 音频尾巴被截断
- 文件时长偏短
- 直播最后一小段内容丢失

所以 `flushEncoders()` 是录制闭环中不可缺少的一环。

---

## 22. `av_write_trailer()`：为什么 MP4 结尾尤其依赖它

主循环结束后：

```cpp
if (m_fmtCtxFile) av_write_trailer(m_fmtCtxFile);
if (m_fmtCtxStream) av_write_trailer(m_fmtCtxStream);
```

### 22.1 trailer 在做什么

可以把它理解成“正式收尾”：
- 写入容器尾部元数据
- 更新索引
- 完成文件结构闭合
- 刷新缓冲中的剩余内容

### 22.2 为什么对 MP4 尤其重要

MP4 的索引/元信息常常依赖收尾阶段补全。

如果没有 trailer：
- 文件可能打不开
- 时长显示不对
- 拖动播放失败
- 有时播放器直接认不出来

### 22.3 对直播流也有意义吗

有。

虽然直播不是传统文件，但正常结束时：
- 发送尾部信号
- 关闭输出
- 让服务端知道这条流已经完整结束

仍然是合理且必要的。

---

## 23. 为什么 `softStop`、`flush`、`trailer` 必须连成一套

如果只做其中一部分，体验和稳定性都会差很多。

### 23.1 只 soft stop，不 flush

后果：
- 队列虽然处理完了
- 但编码器内部缓存没吐干净
- 最后几帧还是会丢

### 23.2 flush 了，不写 trailer

后果：
- 编码数据都在
- 但容器结构未闭合
- MP4 仍然可能坏掉

### 23.3 写 trailer，但前面硬停

后果：
- 音频来不及淡出
- 队列中的帧来不及消费
- 生成的是“结构完整但内容突兀”的文件

所以正确闭环是：

```text
软停止请求
   ↓
停止接收新数据
   ↓
处理剩余队列 + 音频淡出
   ↓
flush 编码器缓存
   ↓
write trailer
   ↓
closeResources
```

这才是项目当前实现有成品感的原因之一。

---

## 24. 为什么本项目的同步策略足够实用，但不是唯一方案

项目当前策略可以概括为：

1. 第一帧视频定义启动零点
2. 视频 PTS 基于采集时间
3. 音频按样本数连续编码
4. 音频始终追赶视频进度
5. 输出前做时间基转换
6. RTMP 路径再做严格单调修正

### 24.1 这种策略的优点

- 实现简单清晰
- 很适合桌面录制
- 对采集线程波动容忍度高
- 不依赖过度复杂的外部时钟系统

### 24.2 它的局限

如果以后要做更复杂的场景，例如：
- 暂停/继续录制
- 可变帧率更严格控制
- 多路远端音视频合流
- 精细 A/V drift 校正

那可能需要更复杂的主时钟模型。

但对当前项目来说，这套方案已经非常够用，而且工程性很强。

---

## 25. 从工程角度重新理解第10课

到这里你会发现，本课其实不只是“FFmpeg API 教程”，而是在解释下面这件事：

> **如何把来自不同线程、不同类型、不同节奏的数据，组织成一条对文件播放器和 RTMP 服务端都合法的媒体时间轴。**

这也是为什么音视频开发往往比普通 GUI 开发更难：
- 你处理的不只是数据本身
- 还要处理数据“什么时候出现”
- 以及它在别人的时间体系里“该什么时候被看见”

而 `FFmpegRecorder.cpp` 在这一点上，已经体现出很典型的真实项目设计思路。

---

## 26. 本课应重点掌握的代码片段

### 26.1 视频 PTS 生成

```cpp
qint64 diffMs = frameData.timestamp;
m_vFrame->pts = av_rescale_q(diffMs * 1000, {1, 1000000}, m_vCtx->time_base);
```

### 26.2 音频 PTS 生成

```cpp
m_aFrame->pts = av_rescale_q(m_totalAudioSamples,
                             {1, m_aCtx->sample_rate},
                             m_aCtx->time_base);
```

### 26.3 音频追视频

```cpp
qint64 targetSamples = isFadingOut
    ? finalAudioSampleEnd
    : av_rescale_q(lastVideoPts, {1, 1000}, {1, m_aCtx->sample_rate});
```

### 26.4 时间基转换

```cpp
av_packet_rescale_ts(filePkt, srcTb, outStream->time_base);
```

### 26.5 RTMP 单调性修正

```cpp
if (streamPkt->dts < *lastDts) {
    streamPkt->dts = *lastDts + 1;
}
if (streamPkt->pts < streamPkt->dts) {
    streamPkt->pts = streamPkt->dts;
}
```

### 26.6 收尾 flush

```cpp
avcodec_send_frame(m_vCtx, nullptr);
avcodec_send_frame(m_aCtx, nullptr);
```

### 26.7 容器收尾

```cpp
if (m_fmtCtxFile) av_write_trailer(m_fmtCtxFile);
if (m_fmtCtxStream) av_write_trailer(m_fmtCtxStream);
```

---

## 27. 本课作业

1. **时间轴推导题**：假设视频当前 `lastVideoPts = 2500ms`，音频采样率为 `48000`，请计算音频应该至少编码到多少个样本点，并说明 `while (m_totalAudioSamples < targetSamples)` 为什么能让音频追上视频。

2. **容器理解题**：解释 `AVCodecContext`、`AVStream`、`AVFormatContext` 三者分别负责什么。为什么不能只初始化编码器、不初始化 `AVFormatContext` 就直接得到可播放 MP4？

3. **双输出题**：为什么 `writePacket()` 里必须 `av_packet_clone()`？如果直接把同一个 `AVPacket` 先写文件再写直播，会在哪一步出现问题？

4. **直播稳定性题**：解释为什么 RTMP 比本地 MP4 更依赖“时间戳单调”。如果去掉 `streamPkt->dts` 和 `streamPkt->pts` 的强制修正，最可能出现什么现象？

5. **收尾流程题**：分别说明 `soft stop`、`flushEncoders()`、`av_write_trailer()` 的作用。少掉其中任意一步，最终结果会出现什么缺陷？

---

*上一课：[第9课：FFmpeg编码主流程与音视频队列](./第9课：FFmpeg编码主流程与音视频队列.md)*  
*下一课：[第11课：RTMP直播推流](./第11课：RTMP直播推流.md)*
