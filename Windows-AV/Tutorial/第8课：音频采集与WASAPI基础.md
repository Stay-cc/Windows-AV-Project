# 第8课：音频采集与 WASAPI 基础

> **目标**：深入理解项目如何使用 WASAPI 采集麦克风与系统声音，掌握 `IMMDeviceEnumerator`、`IAudioClient`、`IAudioCaptureClient` 的职责，理解共享模式、Loopback 回采、PCM Float32 统一格式，以及为什么这个项目把音频采集设计成独立线程。  
> **对应文件**：`WasapiCapturer.h`、`WasapiCapturer.cpp`

---

## 1. 音频模块在项目中的作用

在这个项目里，视频采集和音频采集是分开的：

- 视频：`DxGiCapturer` / `WgcCapturer`
- 音频：`WasapiCapturer`
- 编码混流：`FFmpegRecorder`

`WasapiCapturer` 的职责非常纯粹：

1. 打开一个音频设备
2. 持续读取 PCM 数据
3. 计算实时音量电平
4. 通过信号把音频块推送给 `FFmpegRecorder`

所以它是一个典型的**生产者线程**：

```text
WASAPI 设备
   ↓
WasapiCapturer::run()
   ├─ emit volumeLevelChanged(rms)   → UI 电平显示
   └─ emit audioDataReady(...)       → FFmpegRecorder 编码混音
```

项目里实际会创建两类音频采集器：
- **麦克风采集** `Microphone`
- **系统回采** `SystemLoopback`

这就是为什么头文件里定义了：

```cpp
enum DeviceType { Microphone, SystemLoopback };
```

---

## 2. 为什么项目选择 WASAPI？

WASAPI（Windows Audio Session API）是 Windows 原生音频采集/播放接口。它的几个优势非常适合录屏软件：

1. **原生支持系统输出回采（Loopback）**  
   可以直接录到“电脑正在播放的声音”。

2. **能获取默认设备和指定设备**  
   方便做“切换麦克风”“切换扬声器输出源”的 UI。

3. **共享模式兼容性好**  
   不需要独占硬件。

4. **和 FFmpeg 很容易对接**  
   拿到 PCM 后即可送编码器。

所以这个项目没有用 Qt Multimedia 做音频录制主链路，而是直接走 Windows 原生 API，控制力更强。

---

## 3. WasapiCapturer 类结构

头文件结构很清晰：

```cpp
class WasapiCapturer : public QThread
{
    Q_OBJECT
public:
    enum DeviceType { Microphone, SystemLoopback };

    static QList<AudioDeviceInfo> getAvailableDevices(DeviceType type);
    void setDevice(const QString &deviceId, DeviceType type);
    void stop();

signals:
    void volumeLevelChanged(float level);
    void audioDataReady(const QByteArray &data, int sampleRate, int channels);
    void errorOccurred(QString msg);

protected:
    void run() override;
```

可以看出它继承自 `QThread`，而不是 `QObject + moveToThread`。

### 3.1 为什么这里直接继承 QThread？

因为这个类的模型非常简单：
- 线程启动后，就一直执行 `run()` 里的采集循环
- 没有复杂的槽函数分发需求
- 线程的“主体逻辑”就是音频采集本身

所以这里用 `QThread` 继承是合理的。

这和 `DxGiCapturer` 的设计不同：
- `DxGiCapturer` 需要很多跨线程槽调用，因此适合 `moveToThread`
- `WasapiCapturer` 更像“一个专职工作线程对象”

---

## 4. 成员变量解析

```cpp
bool m_stopRequested;
QString m_deviceId;
DeviceType m_type;

ComPtr<IMMDeviceEnumerator> m_enumerator;
ComPtr<IMMDevice> m_device;
ComPtr<IAudioClient> m_audioClient;
ComPtr<IAudioCaptureClient> m_captureClient;
WAVEFORMATEX *m_mixFormat = nullptr;
```

可以分成三层理解。

### 4.1 控制状态

- `m_stopRequested`：采集循环退出标志
- `m_deviceId`：当前指定设备 ID
- `m_type`：麦克风还是系统回采

### 4.2 WASAPI 核心对象

- `IMMDeviceEnumerator`：设备枚举器
- `IMMDevice`：具体音频设备
- `IAudioClient`：音频会话客户端，负责初始化流
- `IAudioCaptureClient`：真正逐包读取采样数据的接口

### 4.3 格式描述

- `m_mixFormat`：当前设备在共享模式下的混音格式

这个 `WAVEFORMATEX` 很关键，因为后续：
- 每个样本占多少字节
- 通道数是多少
- 采样率是多少
- 是 16 位 PCM 还是 32 位 Float

都要靠它来解释原始缓冲区。

---

## 5. 构造、析构与停止逻辑

### 5.1 构造函数

```cpp
WasapiCapturer::WasapiCapturer(QObject *parent)
    : QThread(parent), m_stopRequested(false), m_type(Microphone)
{
}
```

默认：
- 停止标志为 `false`
- 类型先设成 `Microphone`

### 5.2 stop()

```cpp
void WasapiCapturer::stop() {
    m_stopRequested = true;
}
```

这里没有立刻强制终止线程，而是设置一个退出标志，让 `run()` 自己在下一轮检测时退出。

这是标准的**优雅停止**模式。

### 5.3 析构函数

```cpp
WasapiCapturer::~WasapiCapturer() {
    stop();
    wait();
    cleanup();
}
```

顺序非常合理：

1. `stop()`：请求线程退出
2. `wait()`：等待 `run()` 返回
3. `cleanup()`：释放 COM / WASAPI 资源

### 5.3.1 为什么不能直接 delete？

如果线程还在 `run()` 循环中，直接销毁对象会导致：
- 成员变量被释放
- 但采集循环还在继续访问它们
- 最终崩溃

所以第4课中 `SystemController::restartAudio()` 的 `stop() + wait() + delete` 顺序非常重要。

---

## 6. getAvailableDevices()：枚举音频设备

这是一个静态辅助函数，供 UI 刷新设备列表时使用。

```cpp
QList<AudioDeviceInfo> WasapiCapturer::getAvailableDevices(DeviceType type)
```

### 6.1 最关键的一行：COM 初始化模型

```cpp
HRESULT hrInit = CoInitialize(nullptr);
```

代码里专门写了注释：

```cpp
// 【核心修复】必须使用 CoInitialize(nullptr) (STA)
// 之前使用 CoInitializeEx(MTA) 会导致 Qt 主线程崩溃
```

这是非常有工程价值的一个经验点。

### 6.1.1 为什么这里强调 STA？

Windows 的 COM/音频/部分系统组件，对线程公寓模型很敏感。虽然很多后台线程用 MTA 没问题，但这里是在“获取设备列表”的场景里，如果和 Qt 主线程环境冲突，就可能出现奇怪崩溃。

所以作者最后选择：

- 枚举设备时：`CoInitialize(nullptr)` → 默认 STA
- 采集线程里：也使用 `CoInitialize(nullptr)`

这是项目在实际调试后得出的稳定方案。

### 6.2 创建设备枚举器

```cpp
ComPtr<IMMDeviceEnumerator> enumerator;
HRESULT hr = CoCreateInstance(
    __uuidof(MMDeviceEnumerator),
    nullptr,
    CLSCTX_ALL,
    __uuidof(IMMDeviceEnumerator),
    (void**)&enumerator);
```

`IMMDeviceEnumerator` 就是 WASAPI 世界里的“设备管理入口”。

### 6.3 根据类型决定数据流方向

```cpp
EDataFlow dataFlow = (type == Microphone) ? eCapture : eRender;
```

这里的逻辑非常值得记住：

- `Microphone` → `eCapture`
- `SystemLoopback` → `eRender`

为什么系统声音要选 `eRender`？

因为所谓“系统声音录制”，本质上不是去采一个输入设备，而是去**回采输出设备正在播放的内容**。

### 6.4 枚举活动设备

```cpp
hr = enumerator->EnumAudioEndpoints(dataFlow, DEVICE_STATE_ACTIVE, &collection);
```

只拿当前活动设备，避免把禁用/离线设备也放进 UI 列表。

### 6.5 读取设备 ID 和名称

```cpp
device->GetId(&id);
...
props->GetValue(PKEY_Device_FriendlyName, &varName);
```

最终封装成：

```cpp
struct AudioDeviceInfo {
    QString id;
    QString name;
};
```

- `id`：程序内部使用，唯一标识
- `name`：UI 展示给用户看

这和摄像头设备的处理方式一致：**显示名称给人看，实际逻辑用稳定 ID**。

---

## 7. setDevice()：配置将要采集的设备

```cpp
void WasapiCapturer::setDevice(const QString &deviceId, DeviceType type) {
    m_deviceId = deviceId;
    m_type = type;
}
```

这个函数本身很简单，但设计含义很清楚：

- `deviceId` 为空：表示使用默认设备
- `deviceId` 非空：表示使用指定设备

后面在 `initCapture()` 中就会根据它分支。

---

## 8. initCapture()：建立 WASAPI 采集链路

这是音频采集初始化的核心函数。

### 8.1 创建设备枚举器

```cpp
HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                              CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                              (void**)&m_enumerator);
```

和设备枚举阶段一样，首先拿到 `IMMDeviceEnumerator`。

### 8.2 获取默认设备或指定设备

```cpp
if (m_deviceId.isEmpty()) {
    EDataFlow flow = (m_type == Microphone) ? eCapture : eRender;
    hr = m_enumerator->GetDefaultAudioEndpoint(flow, eConsole, &m_device);
} else {
    hr = m_enumerator->GetDevice(m_deviceId.toStdWString().c_str(), &m_device);
}
```

### 8.2.1 默认设备路径

如果用户没指定 ID，就根据类型选择默认设备：
- 麦克风：默认输入设备
- 系统声音：默认输出设备

### 8.2.2 指定设备路径

如果传了设备 ID，就按 ID 精确打开。

这就是 UI 中“下拉框切换音频设备”最终落到的底层逻辑。

---

### 8.3 激活 IAudioClient

```cpp
hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_audioClient);
```

`IAudioClient` 可以理解为：
- 与设备建立一个音频流会话
- 确定共享模式/格式/缓冲参数
- 之后再从它拿采集服务接口

### 8.4 获取混音格式

```cpp
hr = m_audioClient->GetMixFormat(&m_mixFormat);
```

这里拿到的是**共享模式下系统混音引擎使用的格式**。

通常会是：
- Float32
- 某个采样率（如 48000 / 44100）
- 某个通道数（通常 2）

项目里没有强行要求设备输出某个固定格式，而是先接受系统混音格式，再在后续统一处理。

### 8.4.1 为什么用混音格式很方便？

因为共享模式下，WASAPI 最自然、兼容性最好的路径就是直接接受系统混音引擎给出的格式。

这样避免了：
- 格式不支持
- 初始化失败
- 还得自己做更多协商

---

### 8.5 设置 Loopback 标志

```cpp
DWORD flags = (m_type == SystemLoopback) ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
```

这是“录系统声音”的关键。

#### 8.5.1 Loopback 的本质

它不是去采某个麦克风，而是告诉 WASAPI：

> 请把这个输出设备正在播放、即将送往扬声器/耳机的音频流复制一份给我。

所以：
- 麦克风采集：`flags = 0`
- 系统回采：`flags = AUDCLNT_STREAMFLAGS_LOOPBACK`

这正是录屏软件最核心的音频能力之一。

---

### 8.6 共享模式初始化

```cpp
hr = m_audioClient->Initialize(
    AUDCLNT_SHAREMODE_SHARED,
    flags,
    REFTIMES_PER_SEC,
    0,
    m_mixFormat,
    nullptr);
```

参数重点如下：

#### 8.6.1 `AUDCLNT_SHAREMODE_SHARED`

表示共享模式。

优点：
- 不独占设备
- 兼容性最好
- 普通桌面应用最常用

这很符合录屏软件场景，因为用户可能同时：
- 开着会议软件
- 开着播放器
- 录屏程序也在采集

#### 8.6.2 `flags`

决定是否为 loopback。

#### 8.6.3 `REFTIMES_PER_SEC`

```cpp
#define REFTIMES_PER_SEC 10000000
```

这是 100ns 单位下的 1 秒，作为缓冲时长参数传入。

也就是告诉 WASAPI：
- 希望使用大约 1 秒量级的共享缓冲配置

它不是“每秒才读一次”，而是内部缓冲时间单位的设置。

#### 8.6.4 使用 `m_mixFormat`

直接按系统混音格式初始化，最稳妥。

---

### 8.7 获取 IAudioCaptureClient

```cpp
hr = m_audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&m_captureClient);
```

至此，采集链路完整建立：

```text
IMMDeviceEnumerator
   ↓
IMMDevice
   ↓ Activate
IAudioClient
   ↓ GetService
IAudioCaptureClient
```

其中真正负责“读音频包”的是 `IAudioCaptureClient`。

---

## 9. run()：音频采集线程主循环

### 9.1 在线程内初始化 COM

```cpp
CoInitialize(nullptr);
```

这一步和前面的设备枚举思路一致：在当前线程里建立 COM 环境。

### 9.2 初始化失败处理

```cpp
if (!initCapture()) {
    emit errorOccurred("Failed to initialize WASAPI");
    cleanup();
    CoUninitialize();
    return;
}
```

如果初始化失败，就通过信号把错误抛给上层，而不是静默失败。

### 9.3 启动音频客户端

```cpp
m_audioClient->Start();
m_stopRequested = false;
```

注意：
- `initCapture()` 只是建链路
- `Start()` 才是真正开始流动音频数据

---

## 10. 主循环：GetNextPacketSize → GetBuffer → ReleaseBuffer

这是 WASAPI 采集的标准三步曲。

```cpp
while (!m_stopRequested) {
    UINT32 packetLength = 0;
    HRESULT hr = m_captureClient->GetNextPacketSize(&packetLength);

    if (SUCCEEDED(hr) && packetLength > 0) {
        ...
        hr = m_captureClient->GetBuffer(&pData, &numFramesAvailable, &flags, nullptr, nullptr);
        ...
        m_captureClient->ReleaseBuffer(numFramesAvailable);
    } else {
        QThread::msleep(5);
    }
}
```

### 10.1 `GetNextPacketSize`

先问：
> 当前有没有一包可读音频？

### 10.2 `GetBuffer`

如果有，就拿到：
- `pData`：原始音频数据指针
- `numFramesAvailable`：可用帧数
- `flags`：静音等状态标志

### 10.3 `ReleaseBuffer`

处理完后必须归还缓冲区。

如果忘了 `ReleaseBuffer`，后续采集状态会出问题。

---

## 11. 音频格式统一：为什么统一输出 Float32？

项目发给 `FFmpegRecorder` 的信号定义是：

```cpp
void audioDataReady(const QByteArray &data, int sampleRate, int channels);
```

代码注释明确写了：

```cpp
// data: PCM数据 (Float32)
```

也就是说，不管底层设备原始格式是：
- 32 位浮点
- 16 位整数

在离开 `WasapiCapturer` 之前，都会统一成 **Float32 PCM**。

这非常重要，因为它让后面的混音与编码模块不用反复判断设备原始格式。

---

## 12. 处理静音与不同位深

### 12.1 静音标志

```cpp
bool isSilent = (flags & AUDCLNT_BUFFERFLAGS_SILENT);
```

WASAPI 会告诉你这包是不是“逻辑静音”。如果是静音，就没必要读真实数据，直接写零即可。

### 12.2 如果本来就是 32 位浮点

```cpp
if (m_mixFormat->wBitsPerSample == 32 &&
    (m_mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
     m_mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE)) {
    if (isSilent) memset(outPtr, 0, floatData.size());
    else memcpy(outPtr, pData, floatData.size());
}
```

这是最理想路径：
- 设备/系统本来就给 Float32
- 直接拷贝即可

### 12.3 如果是 16 位 PCM

```cpp
else if (m_mixFormat->wBitsPerSample == 16) {
    short* inPtr = (short*)pData;
    int totalSamples = numFramesAvailable * m_mixFormat->nChannels;
    for (int i = 0; i < totalSamples; ++i) {
        if (isSilent) outPtr[i] = 0.0f;
        else outPtr[i] = inPtr[i] / 32768.0f;
    }
}
```

这里做的是经典的 PCM16 → Float32 归一化：

```text
[-32768, 32767] → approximately [-1.0, 1.0)
```

### 12.4 其他未知格式

```cpp
else {
    memset(outPtr, 0, floatData.size());
}
```

如果格式不认识，就输出静音，而不是冒险按错误格式解释内存。

这是一种保守但安全的工程选择。

---

## 13. 计算音量电平：RMS 近似

项目不仅要采音频，还要驱动 UI 上的音量条，因此在采集线程里顺手计算了音量。

```cpp
double sum = 0;
int totalSamples = numFramesAvailable * m_mixFormat->nChannels;
int step = (totalSamples > 500) ? 5 : 1;
for(int i=0; i<totalSamples; i+=step) {
    sum += outPtr[i] * outPtr[i];
}
float rms = (totalSamples > 0) ? sqrt(sum / (totalSamples/step)) : 0.0f;
```

### 13.1 为什么用 RMS？

RMS（均方根）能够较好反映音频能量大小，比简单看峰值更稳定，更适合做电平显示。

### 13.2 为什么抽样计算？

```cpp
int step = (totalSamples > 500) ? 5 : 1;
```

如果样本很多，不逐个计算，而是每隔 5 个样本取一个，降低 CPU 开销。

这说明作者很清楚：
- 音量条只需要“视觉上大致正确”
- 不需要为此消耗过多 CPU

这是典型的实时系统优化。

---

## 14. 发信号：UI 电平与编码数据分流

```cpp
emit volumeLevelChanged(rms);
emit audioDataReady(floatData, m_mixFormat->nSamplesPerSec, m_mixFormat->nChannels);
```

同一包音频数据，在这里被分成两条用途：

### 14.1 `volumeLevelChanged`

给 UI 用，只关心“有多响”。

### 14.2 `audioDataReady`

给 `FFmpegRecorder` 用，是真正进入编码链路的 PCM 数据。

这就是 EventBus/控制层分层思想在音频路径上的体现：
- 同一份底层数据
- 根据用途拆成“状态/UI数据”和“业务原始数据”

---

## 15. 为什么空转时 sleep 5ms？

```cpp
else {
    QThread::msleep(5);
}
```

如果当前没有新包可读，线程不能无限空转，否则 CPU 会飙高。

`sleep(5)` 的作用是：
- 给系统一点时间积累下一包音频
- 避免线程 busy loop
- 让 CPU 使用率更平稳

和视频线程里的 `msleep(1)` 是同一个思路，只是音频这里容忍稍长一点的等待。

---

## 16. cleanup()：资源释放顺序

```cpp
void WasapiCapturer::cleanup() {
    if (m_mixFormat) {
        CoTaskMemFree(m_mixFormat);
        m_mixFormat = nullptr;
    }
    m_captureClient.Reset();
    m_audioClient.Reset();
    m_device.Reset();
    m_enumerator.Reset();
}
```

### 16.1 为什么 `m_mixFormat` 要 `CoTaskMemFree`？

因为 `GetMixFormat()` 返回的内存是 COM 分配的，不能用普通 `delete`。

### 16.2 COM 对象为什么用 `ComPtr::Reset()`？

因为这些对象本质上是 COM 接口指针，应该通过 `Release()` 语义释放，`ComPtr` 正好帮我们做这件事。

---

## 17. 麦克风与系统声音路径的差别

虽然 `WasapiCapturer` 只有一个类，但它实际上覆盖两种场景。

| 维度 | 麦克风 | 系统声音 |
|------|--------|----------|
| `DeviceType` | `Microphone` | `SystemLoopback` |
| `EDataFlow` | `eCapture` | `eRender` |
| WASAPI Flags | `0` | `AUDCLNT_STREAMFLAGS_LOOPBACK` |
| 采集对象 | 输入设备 | 输出设备的回采流 |
| 常见用途 | 人声解说 | 桌面播放声音 |

这也是 `SystemController::restartAudio()` 能复用同一套逻辑的原因：
- 只是参数不同
- 底层流程完全一致

---

## 18. WasapiCapturer 与 FFmpegRecorder 的协作

完整链路如下：

```text
WasapiCapturer::run()
   ↓ emit audioDataReady(floatData, sampleRate, channels)
SystemController::restartAudio() 里连接的 lambda
   ↓
FFmpegRecorder::pushAudioData(sourceId, data, sampleRate, channels)
   ↓
编码线程中的音频队列
   ↓
重采样 / 混音 / AAC 编码 / 写入 MP4 或推流
```

其中：
- `sourceId = 0/1` 用于区分系统声和麦克风
- `WasapiCapturer` 只负责“采到干净的 PCM 数据”
- 混音、淡入淡出、编码都不在这里做

这正是良好分层的体现。

---

## 19. 这份实现中的工程亮点

### 19.1 STA 修复是非常关键的经验结论

这不是书本式写法，而是项目在真实调试中踩坑得到的结论：

> 在这里用 `CoInitialize(nullptr)` 比 `CoInitializeEx(...MTA)` 更稳定。

### 19.2 统一输出 Float32 非常明智

这样后面的音频链路更简单，混音和编码模块都少很多分支判断。

### 19.3 Loopback 复用同一套采集器设计很干净

只通过：
- 数据流方向
- 初始化 flag

就把麦克风和系统声音统一进一个类。

### 19.4 音量计算放在采集线程里，避免额外搬运

音量条所需的信息直接在拿到 PCM 时顺手算掉，不再多开一层分析流程。

---

## 20. 本课作业

1. **流程图练习**：画出 `IMMDeviceEnumerator → IMMDevice → IAudioClient → IAudioCaptureClient → QByteArray(Float32)` 的完整流程图，并标注每一步的职责。

2. **Loopback 思考题**：为什么采集“系统声音”时要枚举 `eRender` 设备，而不是 `eCapture` 设备？`AUDCLNT_STREAMFLAGS_LOOPBACK` 在这里起了什么作用？

3. **格式统一题**：项目为什么要把 16 位 PCM 转成 Float32 再发给 `FFmpegRecorder`？如果直接把不同位深原样往后传，会增加哪些复杂度？

4. **扩展设计题**：如果你要给 `WasapiCapturer` 增加“静音检测后自动跳过发送空白包”的优化，你会把逻辑加在哪？这样做对音量显示、音画同步、编码稳定性分别有什么影响？

---

*上一课：[第7课：WinRT摄像头采集](./第7课：WinRT摄像头采集.md)*  
*下一课：[第9课：FFmpeg 编码主流程与音视频队列](./第9课：FFmpeg编码主流程与音视频队列.md)*
