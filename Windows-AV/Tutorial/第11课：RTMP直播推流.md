# 第11课：RTMP 直播推流

> **目标**：深入理解项目如何把已经编码好的 H.264/AAC 数据通过 RTMP 持续推送到直播服务器，掌握 `FFmpegRecorder` 中直播输出初始化、URL 拼接、FLV 封装、网络参数、双输出并行、断流处理，以及为什么直播链路比本地 MP4 更脆弱。  
> **对应文件**：`FFmpegRecorder.cpp`、`RecorderConfig.h`、`SettingsDialog.cpp`

---

## 1. 第11课和前两课的关系

第9课我们学习了：
- 视频如何编码成 H.264
- 音频如何编码成 AAC
- 编码线程如何消费队列
- `AVFrame -> AVPacket` 的编码主流程

第10课我们学习了：
- 编码包如何写入 MP4/FLV
- PTS / DTS / time_base
- `writePacket()` 如何做双路分发
- 为什么直播需要更严格的时间戳单调性修正

第11课要进一步回答一个更贴近真实产品的问题：

> **为什么“能写 FLV”还不等于“能稳定直播”？**

因为直播和本地录制最大的区别在于：
- 文件输出面对的是磁盘
- 直播输出面对的是远端服务器和不稳定网络

也就是说，RTMP 推流不是“把 MP4 写到远程”这么简单，而是一个实时网络传输系统。

可以把这三课连成一条链：

```text
第9课：编码出 H.264/AAC 包
    ↓
第10课：把包组织成合法时间轴并写入容器
    ↓
第11课：把这些包通过 RTMP/FLV 实时送到直播服务器
```

---

## 2. 什么叫 RTMP 推流

RTMP（Real-Time Messaging Protocol）是一种经典直播传输协议。

在当前项目里，直播的意思是：
- 本地采集桌面 / 摄像头 / 音频
- 编码为 H.264 + AAC
- 使用 FLV 作为封装格式
- 通过 RTMP 发送给远端服务器

也就是：

```text
采集 → 编码 → FLV 封装 → RTMP 网络发送 → 直播服务器
```

这里要特别注意：

> **RTMP 是传输协议，FLV 是承载这些音视频包的封装格式。**

所以项目里初始化直播输出时，虽然目标是 RTMP 地址，但代码仍然会显式指定：

```cpp
avformat_alloc_output_context2(&m_fmtCtxStream, nullptr, "flv", fullUrl.toStdString().c_str());
```

这不是写错了，而是因为：
- RTMP 推流时，FFmpeg 通常使用 FLV muxer 组织媒体数据
- 这些 FLV tag 再通过 RTMP 发给服务器

因此在工程语义上更准确的说法是：

> **项目是在用 FLV muxer 做 RTMP 推流。**

---

## 3. 项目里的直播配置从哪里来

先看 `RecorderConfig.h`：`RecorderConfig.h:18`

```cpp
bool enableStreaming = false;
QString rtmpUrl = "";
QString streamKey = "";
```

这三个字段决定了直播功能是否启用，以及推到哪里。

### 3.1 三个字段分别表示什么

- `enableStreaming`：是否开启直播输出
- `rtmpUrl`：RTMP 基础地址，例如 `rtmp://server/app`
- `streamKey`：推流码，用来标识直播会话

在很多平台里：
- 推流服务器地址是公共入口
- 推流码是你的房间/频道身份凭据

项目把它们拆成两个字段，而不是让用户手动输入完整 URL，这样做有两个好处：

1. UI 上更清晰
2. 更方便兼容不同平台的地址拼接规则

### 3.2 UI 如何收集这些配置

在 `SettingsDialog.cpp:62` 开始：

```cpp
m_chkEnableStream = new QCheckBox("Enable Live Streaming (RTMP)");
m_editRtmpUrl = new QLineEdit();
m_editRtmpUrl->setPlaceholderText("rtmp://server-url/app");
m_editStreamKey = new QLineEdit();
m_editStreamKey->setEchoMode(QLineEdit::Password);
```

可以看出作者已经把直播看成正式功能，而不是临时调试项：
- 有独立开关
- 有 RTMP URL 输入框
- 有推流码输入框
- 推流码还用了密码显示模式

这说明设计目标不是“硬编码测试地址”，而是“面向用户配置的直播能力”。

---

## 4. 为什么推流码要单独处理

在 `SettingsDialog.cpp:71`：

```cpp
m_editStreamKey->setEchoMode(QLineEdit::Password);
```

这行看起来只是 UI 细节，其实反映了直播系统里一个很现实的问题：

> **推流码本质上接近凭据。**

如果泄露了推流码，别人可能就能向你的直播间推流。

所以这个项目虽然还没有做到：
- 安全存储
- 脱敏日志
- 凭据管理

但至少已经在 UI 层做了第一步：
- 避免旁观时直接看到完整推流码

这也是工程上常见的思路：
- 先从暴露面最直接的地方减少泄露风险
- 后续再逐步补更完整的凭据保护

---

## 5. 直播开关如何影响录制线程启动

在 `FFmpegRecorder.cpp:45`：

```cpp
if (!m_config.enableFileRecord && !m_config.enableStreaming) {
    qWarning() << "No output selected (File or Stream).";
    return false;
}
```

这段逻辑非常重要。

### 5.1 它在防止什么

它防止用户出现这种配置：
- 不录本地文件
- 也不开直播
- 但仍然启动录制线程

如果没有这段判断，系统会出现一种没有意义的状态：
- 采集还在跑
- 编码还在跑
- 但最终没有任何输出目标

也就是白白消耗 CPU/GPU/内存。

### 5.2 这意味着什么

说明项目已经把“文件输出”和“直播输出”提升为两个并列的一等目标：

```text
至少选择其一，录制才有意义
```

这也是后面双输出架构的基础。

---

## 6. 录制线程如何决定是否初始化直播

看 `FFmpegRecorder.cpp:126` 一段：

```cpp
bool fileOk = true, streamOk = true;
if (m_config.enableFileRecord) {
    if (!initFileOutput()) { emit errorOccurred("File Output Init Failed"); fileOk = false; }
}
if (m_config.enableStreaming) {
    if (!initStreamOutput()) { emit errorOccurred("Streaming Init Failed"); streamOk = false; }
}

if (!fileOk && !streamOk) { closeResources(); return; }
```

### 6.1 这段逻辑体现的设计思想

它不是“直播失败就全挂”，也不是“文件失败就全挂”，而是：

- 文件输出能开就开
- 直播输出能开就开
- 只要至少有一路成功，整个录制仍可继续

这说明项目对真实使用场景有清晰认识：

- 直播经常比本地录制更容易失败
- 网络问题不应该连累本地录制
- 文件路径和直播路径应该是两个故障域

### 6.2 为什么这很像真实产品逻辑

用户真正关心的是：
- 如果直播没推上去，至少录像别丢
- 如果本地磁盘路径有问题，至少直播还能继续

所以这里不是简单的“初始化两个输出”，而是：

> **在启动阶段就把文件录制和直播推流做成了相互独立的能力。**

---

## 7. `initNetwork()`：为什么构造函数里就初始化网络

在 `FFmpegRecorder.cpp:27`：

```cpp
void FFmpegRecorder::initNetwork() {
    avformat_network_init();
}
```

构造函数里会调用：`FFmpegRecorder.cpp:17`

```cpp
initNetwork();
```

### 7.1 它的作用是什么

`avformat_network_init()` 用于初始化 FFmpeg 的网络模块。

对于本地 MP4 录制：
- 不一定依赖网络能力

对于 RTMP 推流：
- 就必须提前准备好网络协议栈支持

### 7.2 为什么放在构造函数更合适

因为 `FFmpegRecorder` 从职责上就是：
- 既负责本地输出
- 又负责网络直播输出

把网络初始化放在对象构造阶段，意味着：
- 这个录制器从创建开始，就具备网络输出能力
- 不需要等到真正开播时再补初始化

这会让运行阶段的逻辑更简单。

---

## 8. `initStreamOutput()` 是直播初始化的入口

直播核心初始化函数在 `FFmpegRecorder.cpp:262`：

```cpp
bool FFmpegRecorder::initStreamOutput()
```

这个函数负责：
- 组装完整 RTMP 地址
- 创建直播输出 `AVFormatContext`
- 建立视频流和音频流
- 设置网络参数
- 打开网络连接
- 写 header

它本质上相当于：

> **把“我要直播”变成一个真正可写入的远端输出目标。**

可以把它拆成下面几步：

```text
1. 生成 fullUrl
2. 创建 FLV 输出上下文
3. 创建视频/音频流
4. 配置网络传输参数
5. 打开 RTMP 连接
6. 写入直播 header
```

---

## 9. 完整 RTMP 地址是怎么拼出来的

在 `FFmpegRecorder.cpp:263`-`267`：

```cpp
QString fullUrl = m_config.rtmpUrl;
if (!m_config.streamKey.isEmpty()) {
    if (!fullUrl.endsWith("/")) fullUrl += "/";
    fullUrl += m_config.streamKey;
}
```

### 9.1 这段代码做了什么

它把：
- 基础地址 `rtmpUrl`
- 推流码 `streamKey`

拼成一个最终地址 `fullUrl`。

例如：

```text
rtmpUrl   = rtmp://server/app
streamKey = live123
```

最终得到：

```text
rtmp://server/app/live123
```

### 9.2 为什么这里要自己补 `/`

因为用户输入并不稳定。

有人可能填：

```text
rtmp://server/app
```

也有人可能填：

```text
rtmp://server/app/
```

如果不做兼容，拼接后可能变成：
- 少斜杠：`appstreamKey`
- 多斜杠：`app//streamKey`

所以这段逻辑本质上是在做一个很轻量的 URL 归一化。

### 9.3 这说明当前项目的假设是什么

当前实现默认直播平台使用的是：

```text
基础地址 + / + 推流码
```

这对多数 RTMP 平台是成立的，但它不是通用真理。

所以你要理解：
- 这是一个实用型实现
- 能覆盖常见平台
- 但还没有做成“支持所有 RTMP 厂商格式”的抽象层

---

## 10. 为什么直播输出明确指定 `flv`

在 `FFmpegRecorder.cpp:272`：

```cpp
avformat_alloc_output_context2(&m_fmtCtxStream, nullptr, "flv", fullUrl.toStdString().c_str());
```

### 10.1 为什么文件输出和直播输出写法不同

文件输出是：

```cpp
avformat_alloc_output_context2(&m_fmtCtxFile, nullptr, nullptr, savePath)
```

因为本地文件可以根据扩展名推断容器。

但直播输出不是一个普通文件路径，而是一个网络地址：
- `rtmp://...`

这个地址本身并不能可靠地告诉 FFmpeg：
- 该用什么 muxer

所以这里必须显式指定：

```cpp
"flv"
```

### 10.2 这一步的工程意义

如果不强制指定 `flv`：
- FFmpeg 可能无法正确判断输出格式
- 或者选错封装器
- 后续写 header / 写包 / 服务端解析都可能失败

所以这不是可选优化，而是 RTMP 输出的关键配置。

---

## 11. 直播为什么仍然要创建 `AVStream`

在 `FFmpegRecorder.cpp:279`-`288`：

```cpp
m_streamVStream = avformat_new_stream(m_fmtCtxStream, nullptr);
avcodec_parameters_from_context(m_streamVStream->codecpar, m_vCtx);
m_streamVStream->time_base = m_vCtx->time_base;
m_streamVStream->codecpar->codec_tag = 0;

m_streamAStream = avformat_new_stream(m_fmtCtxStream, nullptr);
avcodec_parameters_from_context(m_streamAStream->codecpar, m_aCtx);
m_streamAStream->time_base = m_aCtx->time_base;
m_streamAStream->codecpar->codec_tag = 0;
```

### 11.1 为什么推流也需要“建流”

因为 RTMP 不是把裸 H.264/AAC 乱序塞到网络里就行。

服务器也需要知道：
- 这条流里有视频
- 这条流里有音频
- 它们各自是什么 codec
- 参数是什么
- 后续每个包属于哪一路流

而这些信息正是通过 `AVStream` 和 header 组织起来的。

### 11.2 复制编码器参数的意义

`avcodec_parameters_from_context(...)` 会把编码器的重要参数复制到流描述里，例如：
- codec id
- 分辨率
- 采样率
- 声道布局
- extradata

其中最关键的是 extradata：
- H.264 的 SPS/PPS
- AAC 的 AudioSpecificConfig

没有这些，服务器很可能根本不知道后面的压缩包该怎么解释。

---

## 12. 为什么要把 `codec_tag` 设为 0

在 `FFmpegRecorder.cpp:282` 和 `288`：

```cpp
m_streamVStream->codecpar->codec_tag = 0;
m_streamAStream->codecpar->codec_tag = 0;
```

### 12.1 这行代码通常在修什么问题

`codec_tag` 是容器相关的 codec 标记。

但编码器上下文里的某些 tag：
- 可能来自别的容器语义
- 可能不适合当前 FLV muxer
- 可能引发 muxer 警告甚至兼容性问题

所以这里直接清零，交给 muxer 自己决定最合适的 tag。

### 12.2 这类代码的理解方式

这不是“FFmpeg 教程里必须背”的主干知识，更多像是：
- 真正做过容器兼容后留下的稳定性修正
- 防止已有 tag 污染目标封装格式

也就是说，这是典型工程经验代码。

---

## 13. RTMP 传输参数是直播稳定性的关键

在 `FFmpegRecorder.cpp:291`-`296`：

```cpp
AVDictionary* options = nullptr;
av_dict_set(&options, "rtmp_transport", "tcp", 0);
av_dict_set(&options, "rtmp_buffer", "1000", 0);
av_dict_set(&options, "buffer_size", "4096000", 0);
av_dict_set(&options, "rw_timeout", "5000000", 0);
av_dict_set(&options, "tcp_nodelay", "1", 0);
```

这一段是本课最能体现“直播是网络工程”的地方。

### 13.1 `rtmp_transport = tcp`

```cpp
av_dict_set(&options, "rtmp_transport", "tcp", 0);
```

含义：
- 强制 RTMP 走 TCP

为什么合理：
- RTMP 本身常见实现就是基于 TCP
- TCP 有可靠传输、顺序保证
- 对直播推流来说，一般比非可靠传输更符合预期

### 13.2 `rtmp_buffer = 1000`

```cpp
av_dict_set(&options, "rtmp_buffer", "1000", 0);
```

含义：
- 设置 RTMP 侧缓冲约 1000ms

作用：
- 缓和瞬时抖动
- 不至于每个小波动都立刻把链路击穿

代价：
- 增加一些端到端延迟

### 13.3 `buffer_size = 4096000`

```cpp
av_dict_set(&options, "buffer_size", "4096000", 0);
```

含义：
- 扩大发送缓冲区

作用：
- 短时间发送堆积时，更不容易立刻写失败
- 对高码率画面更友好

### 13.4 `rw_timeout = 5000000`

```cpp
av_dict_set(&options, "rw_timeout", "5000000", 0);
```

含义：
- 读写超时设为 5 秒（单位微秒）

作用：
- 网络卡死时不要无限阻塞
- 让程序能更快识别“这条连接已经不正常了”

### 13.5 `tcp_nodelay = 1`

```cpp
av_dict_set(&options, "tcp_nodelay", "1", 0);
```

含义：
- 禁用 Nagle 算法

作用：
- 降低小包合并带来的额外等待
- 更偏向低延迟实时发送

### 13.6 这些参数体现的整体取舍

它们并不是在追求：
- 最低带宽占用
- 最小 CPU
- 最完美吞吐量

而是在追求：

> **直播场景下“尽量稳，同时尽量低延迟”的平衡。**

---

## 14. 为什么 `avio_open2()` 而不是 `avio_open()`

在 `FFmpegRecorder.cpp:298`-`305`：

```cpp
ret = avio_open2(&m_fmtCtxStream->pb, fullUrl.toStdString().c_str(), AVIO_FLAG_WRITE, nullptr, &options);
```

和文件输出对比：
- 本地文件使用 `avio_open()`
- 直播输出使用 `avio_open2()`

### 14.1 两者的关键区别

`avio_open2()` 支持带参数字典打开 I/O。

而直播输出恰恰需要：
- transport
- buffer
- timeout
- tcp_nodelay

这些都要通过 `AVDictionary` 传进去。

所以：

> **不是直播“更高级”，而是网络 I/O 天生就比文件 I/O 需要更多连接参数。**

### 14.2 这也反映了文件和网络的本质差异

文件路径：
- 本地磁盘
- 接口简单
- 可控性高

网络路径：
- 远端连接
- 参数复杂
- 失败模式多
- 需要超时和缓冲控制

所以直播初始化天然更复杂。

---

## 15. `avformat_write_header()` 对直播尤其关键

在 `FFmpegRecorder.cpp:307`：

```cpp
ret = avformat_write_header(m_fmtCtxStream, &options);
```

### 15.1 它做的不是“形式化步骤”

很多人会以为：
- 只要已经拿到编码包
- 直接 write frame 就行

实际上不行。

直播 header 负责把下面这些信息先告诉服务端：
- 这是 FLV/RTMP 流
- 包含视频流和音频流
- 视频是 H.264
- 音频是 AAC
- 参数和 extradata 是什么

服务端只有先收到这些信息，才知道后面的包该怎么解释。

### 15.2 如果 header 写失败会怎样

项目直接把它视为直播初始化失败：
- `printError("Stream Write Header", ret);`
- 返回 `false`

这是合理的，因为 header 都写不通意味着：
- 这条推流会话根本没真正建立成功
- 后面继续写包也没有意义

---

## 16. 为什么要把直播错误打印得这么明确

在 `FFmpegRecorder.cpp:274`、`301`、`311`：

```cpp
printError("Stream Alloc Context", ret);
printError("Stream Connect Failed", ret);
printError("Stream Write Header", ret);
```

### 16.1 这在直播里比本地录制更重要

本地录制失败的原因相对少：
- 路径错误
- 权限不足
- 磁盘问题

而直播失败可能来自：
- 地址错误
- DNS 问题
- 端口被拦截
- 服务器拒绝
- 鉴权失败
- 超时
- 网络中断

如果日志只写一句：

```text
Streaming init failed
```

几乎没法排查。

### 16.2 `av_strerror` 的价值

`printError()` 内部会把 FFmpeg 错误码翻译成人类可读文本。

这让你在调试时可以更快判断：
- 是地址不通
- 是连接被拒绝
- 是协议层握手失败
- 还是写 header 失败

所以直播功能里，清晰日志几乎是刚需。

---

## 17. 当前项目的直播架构不是“单独编码一份”

第10课我们已经看到，`writePacket()` 会把同一个编码包复制后分别送给：
- 文件路径
- 直播路径

在 `FFmpegRecorder.cpp:326` 和 `339`：

```cpp
AVPacket* filePkt = av_packet_clone(pkt);
AVPacket* streamPkt = av_packet_clone(pkt);
```

### 17.1 这意味着什么

直播并没有：
- 再开一套 H.264 编码器
- 再开一套 AAC 编码器

而是：

> **编码一次，输出两次。**

### 17.2 这么做的好处

1. 节省性能
   - 不需要双倍编码负载

2. 时间轴更一致
   - 文件和直播来自同一份编码结果

3. 延迟更低
   - 避免双编码产生额外等待

### 17.3 为什么 clone 还是必须的

因为后面两条路径会各自修改：
- 时间基
- `stream_index`
- 直播路径还会修 PTS/DTS

所以只能共享“编码结果”，不能共享“同一个 AVPacket 实例”。

---

## 18. `writePacket()` 里的直播分支才是真正的推流出口

看 `FFmpegRecorder.cpp:337` 开始：

```cpp
if (m_fmtCtxStream) {
    AVPacket* streamPkt = av_packet_clone(pkt);
    ...
    int ret = av_interleaved_write_frame(m_fmtCtxStream, streamPkt);
}
```

### 18.1 这一步真正发生了什么

当调用：

```cpp
av_interleaved_write_frame(m_fmtCtxStream, streamPkt);
```

对于直播路径来说，它不是简单写文件，而是在做：
- 交织音视频包
- 交给 FLV muxer
- 通过底层 IO 发送到 RTMP 连接

也就是说，这一行基本就是：

> **把当前编码包真正送上网。**

### 18.2 为什么叫“推流”其实很形象

因为从程序视角看，就是不停把后续包往远端连接里推进：
- 视频包来了，推一个
- 音频包来了，推一个
- 按时间顺序持续不断地推

---

## 19. 直播为什么一定要交织写入

项目在文件输出和直播输出都使用：

```cpp
av_interleaved_write_frame(...)
```

### 19.1 对直播来说交织更关键

直播服务器和下游播放器都希望看到：
- 视频包和音频包按时间顺序到达
- 而不是某一边长时间连续堆积

如果顺序混乱：
- 播放端等待另一种媒体数据
- 缓冲变大
- 音画不同步更明显
- 某些服务端可能更容易判定异常

### 19.2 为什么直播比文件更在意这个

文件可以“写完以后再整体看”。

而直播是：
- 边生产
- 边发送
- 边被服务器消费

没有事后整理整段流的机会。

所以对直播来说，交织写入几乎是默认要求。

---

## 20. RTMP 路径为什么比 MP4 路径更脆弱

这是整个直播实现里最重要的工程事实。

在 `FFmpegRecorder.cpp:345` 后，作者专门对直播时间戳做了更严格修正：

```cpp
if (streamPkt->dts < *lastDts) {
    streamPkt->dts = *lastDts + 1;
}
if (streamPkt->pts < streamPkt->dts) {
    streamPkt->pts = streamPkt->dts;
}
```

### 20.1 为什么直播服务器更敏感

因为 RTMP 服务器没有“回头修复整段文件”的机会。

它只能：
- 按当前收到的包实时处理
- 一边校验一边转发/缓存

一旦发现：
- DTS 回退
- PTS < DTS
- 时间戳不单调

服务端可能就会：
- 直接报错
- 丢包
- 中断连接
- 导致下游播放异常

### 20.2 为什么本地 MP4 往往更宽容

MP4 muxer 通常能：
- 做更多缓冲
- 做更多整理
- 在整体结构允许范围内接受一些短暂不完美

所以同样一份包：
- 写 MP4 可能还能播
- 推 RTMP 却可能直接断流

这就是“直播更脆弱”的根本原因。

---

## 21. 为什么项目要单独维护直播时间戳状态

在 `FFmpegRecorder.cpp:33`-`34`：

```cpp
m_lastStreamVideoPts = 0; m_lastStreamVideoDts = 0;
m_lastStreamAudioPts = 0; m_lastStreamAudioDts = 0;
```

并且在 `writePacket()` 里分别维护：

```cpp
qint64* lastPts = isVideo ? &m_lastStreamVideoPts : &m_lastStreamAudioPts;
qint64* lastDts = isVideo ? &m_lastStreamVideoDts : &m_lastStreamAudioDts;
```

### 21.1 为什么音频和视频要分开记

因为单调性要求是针对“各自 stream 内部”而言的：
- 视频流要自己递增
- 音频流也要自己递增

并不是要求：
- 音频的 DTS 一定大于前一个视频包
- 或视频的 DTS 一定大于前一个音频包

所以必须分开维护两套状态。

### 21.2 为什么每次开播都要重置

如果不重置：
- 新一轮直播会继承上一次会话的最后时间戳
- 导致开播第一批包就出现异常大偏移
- 甚至被误判为不连续或非法

所以这四个值不是普通缓存，而是：

> **直播会话级状态。**

---

## 22. 直播错误处理为什么是“降级”，不是“全停”

在 `FFmpegRecorder.cpp:363`-`377`：

```cpp
if (ret < 0) {
    ...
    if (ret == -10053 || ret == AVERROR(EPIPE)) {
        qCritical() << "Connection lost. Stopping Stream Output.";
        if (!(m_fmtCtxStream->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&m_fmtCtxStream->pb);
        }
        avformat_free_context(m_fmtCtxStream);
        m_fmtCtxStream = nullptr;
    }
}
```

### 22.1 这里处理的是什么场景

当直播写包失败，并且错误是：
- `-10053`：Windows 上典型的连接中止/中断错误
- `EPIPE`：broken pipe

说明这条 RTMP 连接大概率已经死了。

### 22.2 作者为什么直接关闭直播上下文

因为这时候继续往这条连接写：
- 没有意义
- 可能反复报错
- 可能阻塞线程
- 甚至引发崩溃

所以最务实的做法是：
- 立即停止直播输出
- 把 `m_fmtCtxStream` 置空
- 后面 `writePacket()` 就自动不再进入直播分支

### 22.3 为什么这是好设计

因为：
- 直播失败是高概率外部故障
- 本地录制是更容易保住的结果

所以项目选择：

```text
直播挂了 → 只停直播 → 继续录本地文件
```

这正是一个成熟录制工具该做的事。

---

## 23. 当前项目为什么没有做自动重连

从现有代码看，直播连接断了以后：
- 会关闭直播输出
- 不再继续推
- 但保留本地录制

并没有实现：
- 自动重连
- 退避重试
- 恢复后续推流

### 23.1 这是不是缺陷

从“完整直播产品”角度看，确实还不完整。

但从当前项目阶段看，这个取舍是合理的。

因为自动重连要解决很多额外问题：
- 什么时候重连
- 重连多少次
- 重连期间是否继续缓存包
- 重连成功后时间戳如何续接
- 服务端是否允许无缝续流
- UI 如何提示当前状态

这些都不是加一个循环重试就能解决的。

### 23.2 当前实现优先保证什么

当前项目优先保证的是：
- 不因直播失败拖垮录制线程
- 不因网络故障导致程序崩溃
- 用户至少保住本地录像

这是一个很典型的阶段性工程决策。

---

## 24. 直播参数和编码参数为什么要一起看

RTMP 推流并不只取决于网络参数，还非常依赖编码器输出特性。

在 `FFmpegRecorder.cpp:467`-`486`，视频编码器被配置为：

```cpp
m_vCtx->time_base = {1, 1000000};
m_vCtx->framerate = {m_config.fps, 1};
m_vCtx->gop_size = m_config.fps * 2;
m_vCtx->max_b_frames = 0;
m_vCtx->bit_rate = m_config.videoBitrate;
m_vCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
```

如果使用 `libx264`：

```cpp
av_opt_set(m_vCtx->priv_data, "preset", "ultrafast", 0);
av_opt_set(m_vCtx->priv_data, "tune", "zerolatency", 0);
```

### 24.1 为什么这些设置和直播强相关

- `max_b_frames = 0`
  - 减少重排复杂度
  - 更有利于低延迟

- `bit_rate = m_config.videoBitrate`
  - 决定推流带宽压力

- `AV_CODEC_FLAG_GLOBAL_HEADER`
  - 让 FLV/RTMP 能拿到需要的 codec config

- `ultrafast + zerolatency`
  - 明显偏向实时场景，而不是离线压缩效率

### 24.2 这说明一个核心事实

> **直播不是“网络模块的事”，而是编码 + 封装 + 传输联动的结果。**

---

## 25. 为什么 UI 里的码率设置会直接影响推流成功率

在 `SettingsDialog.cpp:49`-`50`：

```cpp
m_spinBitrate = new QSpinBox();
m_spinBitrate->setRange(1, 100); m_spinBitrate->setSuffix(" Mbps");
```

保存时：`SettingsDialog.cpp:134`

```cpp
m_config.videoBitrate = m_spinBitrate->value() * 1000000;
```

### 25.1 这看起来只是编码设置，其实也影响网络

例如你设成：
- 20 Mbps
- 50 Mbps
- 100 Mbps

编码器当然可能照样输出，
但网络链路未必扛得住。

这会导致：
- 发送缓冲压力增大
- 推流端更容易卡顿
- 服务器接收压力变大
- 弱网下更容易超时或断流

### 25.2 所以直播调参本质上是平衡问题

要同时平衡：
- 清晰度
- 帧率
- 编码复杂度
- 网络带宽
- 延迟
- 稳定性

这也是为什么真正直播软件通常会有大量预设，而不是只给一个“尽量高清”的按钮。

---

## 26. 为什么当前项目的直播 UI 还比较“底层”

现在 `SettingsDialog.cpp` 里直播配置基本就是：
- 开关
- RTMP URL
- Stream Key

这说明当前项目更像：
- 面向开发学习
- 面向具备基本直播概念的用户

而不是完全产品化的直播平台适配界面。

### 26.1 少了哪些典型产品层功能

例如目前还没有：
- 平台预设（B站 / YouTube / Twitch）
- 一键复制测试地址
- 连接测试按钮
- 当前推流状态显示
- 实时上传码率显示
- 重连次数与错误提示面板

### 26.2 但为什么这不影响学习价值

恰恰相反，当前这种更“原始”的实现更适合学习。

因为你能直接看到：
- 直播配置长什么样
- 这些配置如何进入 `RecorderConfig`
- `FFmpegRecorder` 又如何使用这些字段构建网络输出

对初学者来说，这是很好的透明实现。

---

## 27. 当前项目的双输出能力为什么很值得学习

这里再把整个链路串一次：

1. UI 层采集配置
2. `RecorderConfig` 保存直播参数
3. `startRecord()` 根据配置决定是否开文件/直播
4. `initFileOutput()` 初始化 MP4
5. `initStreamOutput()` 初始化 RTMP/FLV
6. `writePacket()` 把同一份编码包同时送往两路输出
7. 直播出错时只关闭直播分支
8. 本地录制继续进行

这条链路体现了一个非常实用的架构思想：

> **把“编码”与“输出目标”解耦。**

也就是说：
- 编码层只负责产出标准压缩包
- 输出层决定包要去哪里

这样你未来就可以继续扩展：
- 文件 + RTMP
- 文件 + RTMP + SRT
- 只录文件
- 只直播

而不需要重写编码主流程。

---

## 28. 如果以后要升级直播能力，可以往哪些方向扩展

基于当前代码，后续最自然的扩展方向包括：

### 28.1 自动重连

- 检测断流
- 延迟若干秒重连
- UI 显示“重连中”

### 28.2 推流状态反馈

- 已连接 / 连接失败 / 已断开
- 当前上传码率
- 当前丢包 / 重试次数

### 28.3 多平台适配

- 平台模板
- 预填 RTMP URL
- 推流码单独保存

### 28.4 更细粒度的低延迟优化

- GOP 更短
- 关键帧周期可调
- 音频缓冲与网络缓冲联动

### 28.5 更完整的异常恢复模型

- 断线期间是否缓存关键帧
- 重连后是否丢弃旧包
- 时间戳如何重新起点或续接

这些扩展都建立在当前这套基础架构之上，所以学懂现在的实现很重要。

---

## 29. 从工程角度重新理解第11课

到这里你会发现，这一课本质上不是在讲“RTMP API 怎么调”，而是在讲：

> **如何把本地实时产生的媒体数据，通过一个高失败概率的网络通道，持续、合法、尽量低延迟地送到远端服务器。**

也就是说，直播推流至少同时涉及三类问题：

1. **媒体问题**
   - 编码格式是否合法
   - 时间戳是否正确

2. **容器问题**
   - FLV header 是否正确
   - 音视频流描述是否完整

3. **网络问题**
   - 连接是否成功
   - 发送是否超时
   - 断流后如何降级

而这个项目的价值就在于：
- 它没有把直播神秘化
- 而是用一个相对清晰的 `initStreamOutput() + writePacket()` 结构，把直播拆成了可以理解的工程步骤

---

## 30. 本课应重点掌握的代码片段

### 30.1 直播配置字段

```cpp
bool enableStreaming = false;
QString rtmpUrl = "";
QString streamKey = "";
```

### 30.2 拼接完整推流地址

```cpp
QString fullUrl = m_config.rtmpUrl;
if (!m_config.streamKey.isEmpty()) {
    if (!fullUrl.endsWith("/")) fullUrl += "/";
    fullUrl += m_config.streamKey;
}
```

### 30.3 创建 RTMP/FLV 输出上下文

```cpp
avformat_alloc_output_context2(&m_fmtCtxStream, nullptr, "flv", fullUrl.toStdString().c_str());
```

### 30.4 创建直播音视频流

```cpp
m_streamVStream = avformat_new_stream(m_fmtCtxStream, nullptr);
avcodec_parameters_from_context(m_streamVStream->codecpar, m_vCtx);

m_streamAStream = avformat_new_stream(m_fmtCtxStream, nullptr);
avcodec_parameters_from_context(m_streamAStream->codecpar, m_aCtx);
```

### 30.5 RTMP 网络参数

```cpp
av_dict_set(&options, "rtmp_transport", "tcp", 0);
av_dict_set(&options, "rtmp_buffer", "1000", 0);
av_dict_set(&options, "buffer_size", "4096000", 0);
av_dict_set(&options, "rw_timeout", "5000000", 0);
av_dict_set(&options, "tcp_nodelay", "1", 0);
```

### 30.6 打开网络连接

```cpp
ret = avio_open2(&m_fmtCtxStream->pb, fullUrl.toStdString().c_str(), AVIO_FLAG_WRITE, nullptr, &options);
```

### 30.7 写直播 header

```cpp
ret = avformat_write_header(m_fmtCtxStream, &options);
```

### 30.8 直播写包与断流降级

```cpp
int ret = av_interleaved_write_frame(m_fmtCtxStream, streamPkt);

if (ret == -10053 || ret == AVERROR(EPIPE)) {
    if (!(m_fmtCtxStream->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&m_fmtCtxStream->pb);
    }
    avformat_free_context(m_fmtCtxStream);
    m_fmtCtxStream = nullptr;
}
```

---

## 31. 本课作业

1. **地址拼接题**：假设 `rtmpUrl = rtmp://localhost/live`，`streamKey = room001`，请写出 `fullUrl` 的最终结果，并说明为什么代码里要判断 `fullUrl.endsWith("/")`。

2. **初始化流程题**：请按顺序说明 `initStreamOutput()` 里从“用户打开直播开关”到“直播服务器准备接收媒体包”为止，中间至少经历了哪些关键步骤。

3. **协议理解题**：解释为什么项目在 RTMP 推流时仍然要显式指定 `"flv"` 作为输出格式。RTMP 和 FLV 在这里分别扮演什么角色？

4. **稳定性题**：解释 `rtmp_buffer`、`rw_timeout`、`tcp_nodelay` 各自大致解决什么问题。它们之间为什么体现的是“稳定性和延迟之间的平衡”？

5. **容错题**：阅读 `writePacket()` 中直播错误处理逻辑，说明为什么作者在连接断开后选择“停止直播但继续本地录制”，而不是直接让整个录制线程退出。

---

*上一课：[第10课：FFmpeg混流与时间戳同步](./第10课：FFmpeg混流与时间戳同步.md)*  
*下一课：[第12课：Qt无边框窗口与自定义控件](./第12课：Qt无边框窗口与自定义控件.md)*
