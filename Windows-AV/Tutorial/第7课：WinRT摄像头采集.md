# 第7课：WinRT 摄像头采集

> **目标**：深入理解项目如何使用 WinRT `MediaCapture` 采集摄像头画面，掌握 `MediaFrameReader` 回调模型、像素格式协商、`SoftwareBitmap` 到 D3D11 纹理的上传流程，以及为什么当前实现选择 **CPU 路线** 而不是直接走 GPU Surface。  
> **对应文件**：`WinRTCamera.h`、`WinRTCamera.cpp`

---

## 1. 摄像头模块在项目中的定位

前面几课我们已经学过：
- **DXGI / WGC** 负责采集桌面或窗口
- **FFmpegRecorder** 负责编码
- **SystemController** 负责线程协调

而 `WinRTCamera` 的职责不是替代主画面采集，而是：

- 从摄像头读取视频帧
- 转成项目统一可用的 **D3D11 Shader Resource View（SRV）**
- 作为一个“图层纹理”交给 `DxGiCapturer` 参与 GPU 合成

也就是说，摄像头在这个项目里本质上是一个 **Overlay Source**，不是主采集源。

可以把它理解成：

```text
摄像头设备
   ↓
WinRTCamera
   ↓ 输出 SRV
DxGiCapturer::renderTexture()
   ↓
与桌面/窗口纹理一起做 GPU 合成
   ↓
最终进入预览 / 录制 / 推流
```

这就是为什么 `WinRTCamera` 最终暴露的不是 `QImage`，也不是 `ID3D11Texture2D*`，而是：

```cpp
ComPtr<ID3D11ShaderResourceView> getLatestFrame();
```

因为项目后面的渲染管线最想要的就是 **可直接绑定到像素着色器的 SRV**。

---

## 2. WinRTCamera 类结构

先看头文件中的核心结构：

```cpp
class WinRTCamera : public QObject
{
    Q_OBJECT
public:
    bool init(ID3D11Device* device);
    bool start(const QString& deviceId);
    void stop();

    ComPtr<ID3D11ShaderResourceView> getLatestFrame();

private:
    void onFrameArrived(MediaFrameReader const& sender,
                        MediaFrameArrivedEventArgs const& args);
```

成员变量可以分成四组。

### 2.1 基础状态

```cpp
QMutex m_mutex;
QString m_currentDeviceId;
std::atomic<bool> m_running{false};
```

作用：
- `m_mutex`：保护当前帧纹理/视图
- `m_currentDeviceId`：记录当前正在使用的摄像头 ID
- `m_running`：表示采集是否处于运行状态

### 2.2 D3D 资源

```cpp
ComPtr<ID3D11Device> m_d3dDevice;
ComPtr<ID3D11DeviceContext> m_d3dContext;
winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_winrtDevice{ nullptr };
```

这里同时保存了两套“设备视角”：
- 传统 D3D11 世界：`ID3D11Device`、`ID3D11DeviceContext`
- WinRT 世界：`IDirect3DDevice`

这和第6课 WGC 的桥接思路是一样的：**WinRT API 和传统 D3D11 API 之间要做设备包装**。

### 2.3 WinRT 采集资源

```cpp
MediaCapture m_mediaCapture{ nullptr };
MediaFrameReader m_frameReader{ nullptr };
winrt::event_token m_frameToken;
```

三者职责：
- `MediaCapture`：摄像头采集的总入口
- `MediaFrameReader`：逐帧回调读取器
- `m_frameToken`：注销回调时使用

### 2.4 当前帧缓存

```cpp
ComPtr<ID3D11Texture2D> m_copyTexture;
ComPtr<ID3D11Texture2D> m_stagingTexture;
ComPtr<ID3D11ShaderResourceView> m_currentSRV;
std::atomic<int> m_frameCount{0};
```

真正被项目使用的是：
- `m_copyTexture`：GPU 纹理
- `m_currentSRV`：对应 SRV

注意：`m_stagingTexture` 在当前文件中并没有实际参与主流程，属于保留字段。

---

## 3. init()：绑定外部 D3D 设备

### 3.1 为什么摄像头模块不自己创建设备？

```cpp
bool WinRTCamera::init(ID3D11Device* device) {
    if (!device) return false;
    m_d3dDevice = device;
    m_d3dDevice->GetImmediateContext(&m_d3dContext);
    ...
}
```

这里没有自己调用 `D3D11CreateDevice`，而是直接复用外部传入的设备。

原因很重要：

1. **全项目统一 GPU 资源上下文**  
   摄像头纹理最终要交给 `DxGiCapturer` 合成，如果设备不是同一个，资源共享会更复杂。

2. **避免跨设备拷贝**  
   同一个 `ID3D11Device` 下创建的纹理和 SRV，可以直接进入已有渲染管线。

3. **线程与生命周期更统一**  
   摄像头预热、图层渲染、主画面合成都围绕同一个 D3D 设备进行。

### 3.2 包装成 WinRT 的 IDirect3DDevice

```cpp
ComPtr<IDXGIDevice> dxgiDevice;
m_d3dDevice.As(&dxgiDevice);
winrt::com_ptr<IInspectable> inspectable;
CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put());
if (inspectable) {
    m_winrtDevice = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
}
```

这一步和 WGC 非常像：

```text
ID3D11Device
   ↓ As(IDXGIDevice)
IDXGIDevice
   ↓ CreateDirect3D11DeviceFromDXGIDevice
IDirect3DDevice (WinRT)
```

虽然当前 `WinRTCamera.cpp` 主采集流程是 CPU 内存路线，但项目作者仍然保留了 `m_winrtDevice`，说明设计上已经考虑了 WinRT / D3D 互操作的扩展空间。

---

## 4. start(deviceId)：启动摄像头采集

`start()` 是整个模块最核心的函数。

```cpp
bool WinRTCamera::start(const QString& deviceId) {
    if (m_running) stop();
    m_currentDeviceId = deviceId;
    m_frameCount = 0;
    ...
}
```

可以看到，它首先做两件事：
- 如果之前已经在跑，先 `stop()`
- 记录新的设备 ID，并重置帧计数

这是一种典型的“可重入启动”设计：**切换摄像头时不要求调用者先手动停掉旧实例**。

---

## 5. MediaCapture 初始化参数解析

### 5.1 创建 MediaCapture

```cpp
m_mediaCapture = MediaCapture();
```

`MediaCapture` 是 WinRT 摄像头/麦克风采集的核心对象。可以把它理解成：
- 摄像头设备连接器
- 采集会话入口
- 帧源和格式控制器的宿主

### 5.2 注册失败回调

```cpp
m_mediaCapture.Failed([this](MediaCapture const&, MediaCaptureFailedEventArgs const& args) {
    qWarning() << "[WinRTCamera] CRITICAL ERROR:" << (int)args.Code()
               << QString::fromStdWString(args.Message().c_str());
});
```

这属于异步错误通道。因为摄像头设备在运行过程中可能突然断开、驱动异常或被系统回收，所以不能只依赖 `InitializeAsync().get()` 的一次性返回值。

### 5.3 设置初始化参数

```cpp
MediaCaptureInitializationSettings settings;
settings.VideoDeviceId(deviceId.toStdWString());
settings.MemoryPreference(MediaCaptureMemoryPreference::Cpu);
settings.StreamingCaptureMode(StreamingCaptureMode::Video);
settings.SharingMode(MediaCaptureSharingMode::SharedReadOnly);
```

这四个参数非常关键。

#### 5.3.1 指定设备 ID

```cpp
settings.VideoDeviceId(deviceId.toStdWString());
```

告诉 WinRT：我要打开哪一个具体摄像头。

这里传入的是 Qt 层枚举出的设备 ID，而不是“友好名称”。这比按名称匹配更可靠，因为名称可能重复或本地化。

#### 5.3.2 MemoryPreference = Cpu

```cpp
settings.MemoryPreference(MediaCaptureMemoryPreference::Cpu);
```

这是本实现最值得注意的设计点之一：**明确要求帧优先以 CPU 可访问的形式提供**。

为什么？因为后面代码走的是：

```text
MediaFrameReader
   ↓
VideoMediaFrame
   ↓
SoftwareBitmap
   ↓
锁内存拿原始字节
   ↓
UpdateSubresource 上传到 D3D11 纹理
```

也就是说，这条路径不是“直接拿 GPU Surface”，而是：

**WinRT CPU 帧 → CPU 颜色转换 → 上传到 GPU**

这样做的好处：
- 兼容性高
- 格式处理更简单
- 不依赖某些更复杂的 Direct3D Surface 互操作
- 更容易强制把各种相机输出格式统一成 BGRA8

代价：
- 会多一次 CPU 参与
- 相比纯 GPU 路线有额外拷贝/转换开销

#### 5.3.3 只采视频

```cpp
settings.StreamingCaptureMode(StreamingCaptureMode::Video);
```

这里只需要摄像头画面，不需要音频，所以选择 `Video`。

#### 5.3.4 SharedReadOnly

```cpp
settings.SharingMode(MediaCaptureSharingMode::SharedReadOnly);
```

这表示以共享只读方式访问设备，降低和其他程序争抢摄像头独占访问的概率。

这很适合桌面录屏软件，因为用户经常希望：
- 摄像头同时被会议软件占用
- 本程序也能读到画面

---

## 6. InitializeAsync().get()：异步 API 的同步用法

```cpp
m_mediaCapture.InitializeAsync(settings).get();
```

这是 C++/WinRT 常见写法：
- API 本身是异步的 `InitializeAsync()`
- 当前项目在这里直接 `.get()`，同步等待完成

### 6.1 为什么这里敢同步等待？

因为这个模块通常工作在非 UI 的渲染/视频线程上下文里，或者在预热阶段异步调度，不直接卡住主界面。

如果在 UI 线程大量使用 `.get()`，就容易造成界面卡顿；但在这个项目里，作者更重视：
- 初始化逻辑简单直接
- 便于在 `start()` 内完整串起“初始化 → 格式协商 → FrameReader 启动”

这是一种工程取舍：**用同步写法换实现清晰度**。

---

## 7. 格式协商：为什么要自己打分选格式？

摄像头并不一定直接输出 BGRA 图像。不同驱动常见格式包括：
- `Bgra8`
- `Nv12`
- `Yuy2`
- `Mjpg`

所以项目没有“拿到第一个格式就用”，而是做了一个评分系统。

### 7.1 扫描所有可用格式

```cpp
auto deviceController = m_mediaCapture.VideoDeviceController();
auto allProperties = deviceController.GetAvailableMediaStreamProperties(MediaStreamType::VideoRecord);
```

拿到所有候选的 `VideoEncodingProperties`。

### 7.2 评分逻辑

```cpp
bool isBgra = (subtype == MediaEncodingSubtypes::Bgra8());
bool isNV12 = (subtype == MediaEncodingSubtypes::Nv12());
bool isYUY2 = (subtype == MediaEncodingSubtypes::Yuy2());
bool isMJPG = (subtype == MediaEncodingSubtypes::Mjpg());

if (isBgra) score += 3000;
else if (isNV12) score += 2000;
else if (isYUY2) score += 1000;
else if (isMJPG) score += 500;
else continue;
```

这里直接体现了作者的偏好顺序：

1. **BGRA8 最优**：无需再做颜色空间转换，最省事
2. **NV12 次优**：常见高效 YUV 格式
3. **YUY2 再次**：也是常见原始视频格式
4. **MJPG 较低**：需要额外解码，处理更重

### 7.3 分辨率和帧率偏好

```cpp
if (w == 1920 && h == 1080) score += 100;
else if (w == 1280 && h == 720) score += 80;

if (std::abs(fps - 30.0) < 1.0) score += 50;
if (std::abs(fps - 60.0) < 1.0) score += 60;
```

说明作者不仅关心“能不能用”，还在做“尽量选更适合录制/直播的格式”。

实际偏好大概是：
- 格式正确性/处理成本优先
- 再考虑分辨率
- 再考虑帧率

### 7.4 应用最佳格式

```cpp
if (bestFormat) {
    deviceController.SetMediaStreamPropertiesAsync(
        MediaStreamType::VideoRecord,
        bestFormat).get();
}
```

这一步相当于告诉驱动：
> 后续请按我挑选的这一档格式输出。

这会极大影响后面 `SoftwareBitmap` 的颜色正确性、转换成本和画面稳定性。

---

## 8. 选择 FrameSource：为什么只取 Color 源？

```cpp
auto frameSources = m_mediaCapture.FrameSources();
MediaFrameSource selectedSource = nullptr;
for (auto pair : frameSources) {
    auto source = pair.Value();
    if (source.Info().SourceKind() == MediaFrameSourceKind::Color) {
        selectedSource = source;
        break;
    }
}
if (!selectedSource) return false;
```

一个摄像头设备可能暴露多个源，例如：
- Color
- Depth
- Infrared
- Metadata

而桌面录屏中的摄像头小窗显然只需要彩色图像，所以这里明确锁定：

```cpp
MediaFrameSourceKind::Color
```

这样可避免拿到深度流之类不适合显示的内容。

---

## 9. MediaFrameReader：事件驱动逐帧采集

### 9.1 创建 Reader

```cpp
m_frameReader = m_mediaCapture.CreateFrameReaderAsync(selectedSource).get();
```

这意味着后面不会手动轮询“有没有新帧”，而是由 `MediaFrameReader` 在每帧到来时通知我们。

### 9.2 注册回调

```cpp
m_frameToken = m_frameReader.FrameArrived({ this, &WinRTCamera::onFrameArrived });
```

### 9.3 启动 Reader

```cpp
MediaFrameReaderStartStatus status = m_frameReader.StartAsync().get();
if (status == MediaFrameReaderStartStatus::Success) {
    m_running = true;
    return true;
}
```

和 WGC 的 `FrameArrived` 很像，摄像头这里也是 **事件驱动模型**：

```text
摄像头驱动产生新帧
   ↓
MediaFrameReader 收到
   ↓
触发 FrameArrived
   ↓
WinRTCamera::onFrameArrived()
```

所以 `WinRTCamera` 不是在外部循环里 `grabFrame()`，而是内部自己缓存最新纹理，外部通过 `getLatestFrame()` 来取。

---

## 10. onFrameArrived()：摄像头帧的完整处理链

这是本课最重要的函数。

```cpp
void WinRTCamera::onFrameArrived(MediaFrameReader const& sender,
                                 MediaFrameArrivedEventArgs const&) {
    if (!m_running) return;
    ...
}
```

整个处理链可以分成 7 步。

---

### 10.1 获取最新帧引用

```cpp
auto frameRef = sender.TryAcquireLatestFrame();
if (!frameRef) return;
auto videoFrame = frameRef.VideoMediaFrame();
if (!videoFrame) return;
```

这里有两个关键词：

- `TryAcquireLatestFrame()`：拿“最新帧”而不是排队拿历史帧
- `VideoMediaFrame()`：从通用帧对象里取出视频帧部分

这也是典型的实时系统设计：**宁可丢旧帧，也优先低延迟**。

---

### 10.2 取出 SoftwareBitmap

```cpp
auto inputBitmap = videoFrame.SoftwareBitmap();
if (!inputBitmap) return;
```

这一步说明当前实现明确走的是：

**VideoMediaFrame → SoftwareBitmap**

也就是 CPU 友好的位图对象，而不是直接从 `Direct3DSurface` 路径取 GPU 纹理。

---

### 10.3 强制转成 BGRA8 Premultiplied

```cpp
SoftwareBitmap bitmapToUpload = nullptr;
if (inputBitmap.BitmapPixelFormat() != BitmapPixelFormat::Bgra8 ||
    inputBitmap.BitmapAlphaMode() != BitmapAlphaMode::Premultiplied)
{
    bitmapToUpload = SoftwareBitmap::Convert(
        inputBitmap,
        BitmapPixelFormat::Bgra8,
        BitmapAlphaMode::Premultiplied);
} else {
    bitmapToUpload = inputBitmap;
}
```

这一步是当前实现的“核心修复”之一。

### 10.3.1 为什么强制统一成 BGRA8？

因为项目后面的 D3D 渲染、SRV 创建、纹理上传，最适合处理统一格式：

```cpp
DXGI_FORMAT_B8G8R8A8_UNORM
```

如果不统一，后面会面临：
- NV12 需要 YUV → RGB 转换
- YUY2 需要拆包和转换
- MJPG 需要先解码
- Alpha 模式可能不一致，叠加时容易出问题

作者选择把复杂度提前收敛在这里：

> 不管摄像头底层给什么格式，到上传前都先转换为 BGRA8 Premultiplied。

### 10.3.2 Premultiplied 的意义

摄像头图层最终要参与 GPU 叠加。若 Alpha 模式不统一，混合结果可能出现边缘发黑、透明异常等问题。

因此这里把 Alpha 模式也标准化。

---

### 10.4 锁定位图缓冲区并拿原始字节

```cpp
auto buffer = bitmapToUpload.LockBuffer(BitmapBufferAccessMode::Read);
auto reference = buffer.CreateReference();
auto byteAccess = reference.as<IMemoryBufferByteAccess>();

uint8_t* pData = nullptr;
uint32_t capacity = 0;
if (SUCCEEDED(byteAccess->GetBuffer(&pData, &capacity)) && pData) {
    ...
}
```

这里使用了一个手动声明的接口：

```cpp
struct IMemoryBufferByteAccess : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetBuffer(uint8_t** value, uint32_t* capacity) = 0;
};
```

### 10.4.1 为什么需要 IMemoryBufferByteAccess？

因为 `SoftwareBitmap` 的底层缓冲区不是直接给你裸指针的。项目通过 `LockBuffer()` + `CreateReference()` + `IMemoryBufferByteAccess` 拿到内存地址，才能把像素数据上传到 D3D11。

这和第6课里手动定义 Interop 接口的思路很像：
- 不一定非得依赖某个完整 SDK 封装
- 只要 ABI 正确，就能桥接到底层能力

---

### 10.5 首帧时懒创建 D3D 纹理和 SRV

```cpp
if (!m_copyTexture || !m_currentSRV) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;

    m_d3dDevice->CreateTexture2D(&desc, nullptr, ...);

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;

    m_d3dDevice->CreateShaderResourceView(...);
}
```

几个重点：

1. **懒创建**：等拿到真实宽高后再建资源
2. **统一格式**：始终用 `DXGI_FORMAT_B8G8R8A8_UNORM`
3. **用途明确**：只用于着色器读取，因此 `BindFlags = D3D11_BIND_SHADER_RESOURCE`

### 10.5.1 为什么不每帧重建纹理？

因为纹理创建很贵。当前设计是：
- 第一次到帧时创建
- 后续每帧只更新内容

这是典型的实时视频优化。

注意：当前代码只在“纹理不存在”时创建，没有针对分辨率变化重建纹理。如果某些摄像头运行中切换分辨率，这里理论上可能需要补一段重建逻辑。

---

### 10.6 UpdateSubresource：把 CPU 像素上传到 GPU

```cpp
int rowPitch = w * 4;
m_d3dContext->UpdateSubresource(m_copyTexture.Get(), 0, nullptr, pData, rowPitch, 0);
```

因为已经统一成 BGRA8，所以每像素固定 4 字节：

```text
rowPitch = width * 4
```

`UpdateSubresource` 的意义是：
- 源数据在 CPU 内存 `pData`
- 目标是 GPU 默认用法纹理 `m_copyTexture`
- 上传完成后，这张纹理就能被渲染管线读取

此时就完成了：

```text
SoftwareBitmap( CPU )
   ↓
raw bytes
   ↓ UpdateSubresource
ID3D11Texture2D( GPU )
   ↓
ID3D11ShaderResourceView
```

---

### 10.7 线程安全缓存当前帧

```cpp
QMutexLocker locker(&m_mutex);
...
m_currentSRV
```

虽然上传纹理和更新 SRV 都发生在回调里，但外部 `DxGiCapturer` 可能同时调用 `getLatestFrame()` 读取，所以这里需要互斥保护。

---

## 11. getLatestFrame()：为什么返回 SRV 而不是 QImage？

```cpp
ComPtr<ID3D11ShaderResourceView> WinRTCamera::getLatestFrame() {
    QMutexLocker locker(&m_mutex);
    return m_currentSRV;
}
```

这一步非常能体现整个项目的架构思想。

如果返回 `QImage`：
- 需要 GPU → CPU 回读
- 主合成阶段还得再 CPU → GPU 上传回去
- 性能非常差

而现在直接返回 `SRV`：
- `DxGiCapturer` 可以直接绑定到像素着色器
- 摄像头帧与主画面共享 GPU 合成管线
- 无需额外来回拷贝

所以这里的关键设计是：

> 摄像头模块内部允许 CPU 参与做格式统一，但模块输出边界必须回到 GPU 纹理视图。

这保证了整体渲染架构的一致性。

---

## 12. stop()：正确关闭摄像头采集

```cpp
void WinRTCamera::stop() {
    m_running = false;
    try {
        if (m_frameReader) {
            m_frameReader.FrameArrived(m_frameToken);
            m_frameReader.StopAsync();
            m_frameReader.Close();
            m_frameReader = nullptr;
        }
        if (m_mediaCapture) {
            m_mediaCapture.Close();
            m_mediaCapture = nullptr;
        }
    } catch (...) {}

    QMutexLocker locker(&m_mutex);
    m_currentSRV.Reset();
    m_copyTexture.Reset();
}
```

关闭顺序可以概括为：

1. 先把 `m_running = false`
2. 注销 `FrameArrived` 回调
3. 停止 `FrameReader`
4. 关闭 `MediaCapture`
5. 释放当前纹理/SRV

### 12.1 为什么要先注销回调？

和 WGC 的停止逻辑同理：
- 如果对象正在销毁
- 回调却还可能被触发
- 就容易访问悬空对象，导致崩溃

所以这一步非常关键。

### 12.2 这里的一个工程细节

`StopAsync()` 在当前代码里没有 `.get()` 等待完成，属于偏“宽松”的停止方式。多数情况下可以工作，但如果要更严格地保证停止完成，再做资源释放，理论上可以同步等待它结束。

---

## 13. 为什么当前实现选择 CPU 路线？

这一课最值得思考的不是“代码怎么写”，而是“为什么作者这样选”。

当前路线是：

```text
摄像头帧
   ↓ WinRT
SoftwareBitmap
   ↓ CPU 颜色转换
BGRA8
   ↓ UpdateSubresource
D3D11 Texture + SRV
```

而另一条可能的路线是：

```text
摄像头帧
   ↓ WinRT Direct3DSurface
直接取 GPU Surface
   ↓ Interop
ID3D11Texture2D / SRV
```

### 13.1 当前 CPU 路线的优点

1. **格式控制简单**  
   所有复杂输入统一转 BGRA8，后面逻辑清晰。

2. **兼容性更好**  
   不同摄像头驱动对 GPU Surface 支持差异较大，而 `SoftwareBitmap` 路线更稳。

3. **调试容易**  
   一旦颜色不对、分辨率异常，更容易在 CPU 侧观察和打印。

4. **便于和 Qt / 内存缓冲打交道**  
   CPU 数据更容易验证。

### 13.2 当前 CPU 路线的缺点

1. **多一次 CPU 拷贝/转换**
2. **高分辨率高帧率下 CPU 开销更大**
3. **理论性能不如纯 GPU 路线**

### 13.3 为什么这个取舍在项目里是合理的？

因为摄像头在本项目中主要是：
- 一个小窗图层
- 不是主桌面采集源
- 分辨率通常低于主画面

所以作者优先选择：

> 稳定、颜色正确、工程实现可控

而不是追求极限性能。

---

## 14. WinRTCamera 如何接入整个系统

结合前几课，可以把完整流程串起来。

### 14.1 预热阶段

`SystemController::prewarmAllCameras()` 会：
- 枚举所有摄像头
- 对每个摄像头创建 `WinRTCamera`
- 调用 `init(rawDevice)`
- 调用 `start(deviceId)`
- 放入 `m_cameraPool`

这样用户点击“添加摄像头”时，不必现场等待初始化。

### 14.2 添加到图层

当用户选择某个摄像头后，`SystemController::onAddCamera()` 会：
- 从池中拿出对应 `WinRTCamera`
- 调用 `m_capturer->addCameraOverlay(id, cam, rect)`

### 14.3 渲染阶段

之后 `DxGiCapturer` 在每帧合成时，会从 `WinRTCamera` 拿到：

```cpp
cam->getLatestFrame()
```

得到一个 `ID3D11ShaderResourceView`，再像普通贴图一样参与：

```text
桌面/窗口主纹理 + 摄像头 SRV + 图片 Overlay SRV
   ↓
统一 renderTexture()
   ↓
输出最终合成画面
```

这就是整个项目里“摄像头也是一种纹理源”的真正含义。

---

## 15. 与 WGC / DXGI 的对比

| 维度 | DXGI / WGC 主采集 | WinRTCamera 摄像头采集 |
|------|------------------|------------------------|
| 数据来源 | 桌面 / 窗口 | 摄像头设备 |
| 主要 API | DXGI / WGC | MediaCapture / MediaFrameReader |
| 帧到达模型 | 轮询或回调 | 回调 |
| 当前实现输出 | `ID3D11Texture2D` / SRV | SRV |
| 中间格式 | 桌面纹理 / WinRT Surface | `SoftwareBitmap` |
| 主要用途 | 主画面 | 叠加图层 |
| 性能优先级 | 高 | 稳定和兼容优先 |

可以看到，三条路线虽然前半段完全不同，但项目在后半段故意把输出统一到 GPU 纹理/SRV，这就是架构设计的价值。

---

## 16. 本课作业

1. **画流程图**：画出 `deviceId → MediaCapture → MediaFrameReader → SoftwareBitmap → BGRA8 → UpdateSubresource → SRV` 的完整数据流图，并标注哪几步发生在 WinRT 世界，哪几步发生在 D3D11 世界。

2. **格式协商分析**：为什么项目把 `Bgra8` 的分数设得最高？如果把 `NV12` 提升到最高分，理论上会对后续流程带来什么影响？

3. **CPU 路线思考题**：当前实现为什么先把摄像头帧变成 `SoftwareBitmap`，再上传到 GPU，而不是直接拿 `Direct3DSurface`？请从兼容性、复杂度、性能三方面分析。

4. **扩展设计题**：如果你要优化 `WinRTCamera`，支持“分辨率变化时自动重建纹理”，你会把检查逻辑放在哪？需要比较哪些字段？重建时要重置哪些资源？

---

*上一课：[第6课：WGC窗口采集与D3D纹理操作](./第6课：WGC窗口采集与D3D纹理操作.md)*  
*下一课：[第8课：音频采集与WASAPI基础](./第8课：音频采集与WASAPI基础.md)*
