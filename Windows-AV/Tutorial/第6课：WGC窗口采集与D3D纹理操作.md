# 第6课：WGC 窗口采集与 D3D 纹理操作

> **目标**：深入理解 Windows Graphics Capture（WGC）的工作机制，掌握 WinRT 与 D3D11 互操作、窗口句柄到 `GraphicsCaptureItem` 的转换、帧池回调模型，以及为什么项目要同时保留 DXGI 与 WGC 两套采集路径。  
> **对应文件**：`WgcCapturer.h`、`WgcCapturer.cpp`、`DxGiCapturer.cpp`（WGC 分支）

---

## 1. 为什么还需要 WGC？

上一课我们讲了 DXGI Desktop Duplication，它非常适合**整屏/显示器采集**。但它并不擅长窗口级捕获，尤其在以下场景：

| 场景 | DXGI 的问题 | WGC 的优势 |
|------|-------------|-----------|
| 只录某个窗口 | 需要先整屏采集，再裁剪窗口区域 | 直接以窗口为采集目标 |
| 窗口被遮挡/移出屏幕 | 裁剪结果可能不完整 | WGC 能捕到窗口的真实内容 |
| 现代 UWP / 部分硬件加速窗口 | 兼容性一般 | 原生支持更好 |
| 鼠标光标叠加 | 需要自己处理 | WGC 可直接捕获光标 |

因此，本项目采用了双路线：
- **显示器采集** → DXGI Output Duplication
- **窗口采集** → WGC（Windows Graphics Capture）

也就是说，`DxGiCapturer` 其实是一个更高层的统一封装，内部根据采集目标类型切换到 DXGI 或 WGC。

---

## 2. WGC 在项目中的位置

### 2.1 类关系

```cpp
// DxGiCapturer.h
class DxGiCapturer : public QObject {
private:
    WgcCapturer* m_wgcCapturer = nullptr;  // 内部持有 WGC 采集器
};
```

`WgcCapturer` 并不是独立线程，也不是 QObject。它只是一个**轻量级 WinRT + D3D11 封装类**，运行在 `DxGiCapturer` 所在的视频线程里。

### 2.2 切换逻辑

在 `DxGiCapturer::startCapture()` 主循环中：

```cpp
bool isMonitor = (m_currentTarget.type == CaptureSourceType::Monitor);
bool isWindow  = (m_currentTarget.type == CaptureSourceType::Window);

if (isMonitor) {
    // 走 DXGI 路线
} else if (isWindow) {
    // 走 WGC 路线
    if (!m_wgcCapturer) {
        HWND root = GetRootWindow(m_currentTarget.windowHandle);
        m_wgcCapturer = new WgcCapturer();
        m_wgcCapturer->init(m_d3dDevice.Get());
        m_wgcCapturer->startCapture(root);
    }
}
```

这意味着：
- `WgcCapturer` 复用了 `DxGiCapturer` 已创建好的 `ID3D11Device`
- 两条采集路径最终都输出为 `ID3D11Texture2D*`
- 后续的 GPU 合成、Staging Texture 回读、FFmpeg 编码完全统一

这就是架构上的高明之处：**采集方式不同，但输出接口统一**。

---

## 3. WgcCapturer 成员变量解析

```cpp
class WgcCapturer {
private:
    ID3D11Device* m_d3dDevice = nullptr;  // 复用外部传入的 D3D11 设备

    // WinRT 世界中的 D3D 设备包装
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_winrtDevice{ nullptr };

    // WGC 核心对象
    GraphicsCaptureItem         m_item{ nullptr };      // 捕获目标（窗口/显示器）
    Direct3D11CaptureFramePool  m_framePool{ nullptr }; // 帧池
    GraphicsCaptureSession      m_session{ nullptr };   // 捕获会话

    // 最近捕获到的一帧（D3D11 纹理）
    ComPtr<ID3D11Texture2D> m_lastFrame;
    std::atomic<bool>       m_hasNewFrame{ false };  // 是否有新帧

    std::mutex              m_mutex;                 // 保护 m_lastFrame
    winrt::event_token      m_frameArrivedToken;     // 用于注销回调
};
```

可以看到，WGC 的核心是三件事：
1. `GraphicsCaptureItem`：我到底要捕哪个窗口？
2. `Direct3D11CaptureFramePool`：系统把采集到的帧放在哪里？
3. `GraphicsCaptureSession`：真正开始和停止捕获的会话对象

---

## 4. WinRT 与 D3D11 的桥接

WGC 是 WinRT API，而 DXGI / D3D11 是传统 COM API，两套系统不能直接互用，需要桥接。

### 4.1 init()：把 D3D11Device 包装成 WinRT 设备

```cpp
void WgcCapturer::init(ID3D11Device* device) {
    m_d3dDevice = device;

    if (m_d3dDevice) {
        // 先从 D3D11Device 获取 IDXGIDevice
        com_ptr<IDXGIDevice> dxgiDevice;
        m_d3dDevice->QueryInterface(__uuidof(IDXGIDevice), dxgiDevice.put_void());

        // 再由 DXGI 设备创建 WinRT 风格的 IDirect3DDevice
        com_ptr<IInspectable> inspectable;
        HRESULT hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put());

        if (SUCCEEDED(hr)) {
            m_winrtDevice = inspectable.as<IDirect3DDevice>();
        }
    }
}
```

### 4.2 为什么不能直接把 `ID3D11Device*` 传给 WGC？

因为 WGC API 的参数类型是：

```cpp
Direct3D11CaptureFramePool::CreateFreeThreaded(
    IDirect3DDevice device,  // WinRT 类型
    ...
)
```

而不是 `ID3D11Device*`。`CreateDirect3D11DeviceFromDXGIDevice` 是微软官方提供的桥接函数，用来把传统 DXGI 设备包装成 WinRT 可识别的 `IDirect3DDevice`。

这就是**WinRT 世界**与**传统 COM 世界**的边界。

---

## 5. “核弹级修复”：手动定义 GUID 与 Interop 接口

这是本文件最有工程味道的一部分。

### 5.1 问题背景：依赖地狱

理论上，微软为 WGC 提供了一系列 Interop 头文件和接口定义，例如：
- `IGraphicsCaptureItemInterop`
- `IDirect3DDxgiInterfaceAccess`

但在实际项目中，尤其是 Qt + MSVC + C++/WinRT + Windows SDK 混用时，常常会遇到：
- 某些接口头文件缺失
- GUID 重定义冲突
- 名字空间/ABI 版本冲突
- 某些 SDK 版本下头文件不兼容

为了解决这个问题，项目作者没有继续和头文件斗争，而是直接手动定义**最小必要接口**。

### 5.2 手动定义 GUID

```cpp
// [GUID 1] IGraphicsCaptureItemInterop
static const GUID IID_IGraphicsCaptureItemInterop_Local =
    { 0x3628E81B, 0x3CAC, 0x4C60, { 0xB7, 0xF4, 0x23, 0xCE, 0x0E, 0x0C, 0x33, 0x56 } };

// [GUID 2] IDirect3DDxgiInterfaceAccess
static const GUID IID_IDirect3DDxgiInterfaceAccess_Local =
    { 0xA9B3D012, 0x3DF2, 0x4EE3, { 0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1 } };

// [GUID 3] IGraphicsCaptureItem
static const GUID IID_IGraphicsCaptureItem_Local =
    { 0x79C3F95B, 0x31F7, 0x4EC2, { 0xA4, 0x64, 0x63, 0x2E, 0xF5, 0xD3, 0x07, 0x60 } };
```

### 5.3 手动定义接口

```cpp
struct IGraphicsCaptureItemInterop_Local : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateForWindow(HWND window, REFIID riid, void **result) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateForMonitor(HMONITOR monitor, REFIID riid, void **result) = 0;
};

struct IDirect3DDxgiInterfaceAccess_Local : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID iid, void **p) = 0;
};
```

**关键思想**：我不需要完整的微软头文件定义，只需要知道：
- 这个接口的 IID 是什么
- 这个接口有哪些方法
- 方法签名是什么

只要这些 ABI 信息正确，就能通过 `QueryInterface` 正常工作。

这是一种非常典型的**工程级务实修复**：绕过复杂依赖，直接使用最小 ABI 合约。

---

## 6. startCapture(HWND)：从窗口句柄创建捕获会话

这是 WGC 的核心启动流程。

### 6.1 前置检查

```cpp
bool WgcCapturer::startCapture(HWND hwnd) {
    if (!m_d3dDevice || !m_winrtDevice) return false;

    if (!IsWindow(hwnd)) {
        qWarning() << "[WgcCapturer] Invalid HWND.";
        return false;
    }

    if (IsIconic(hwnd)) {
        qWarning() << "[WgcCapturer] Window is minimized, cannot capture.";
        return false;
    }
```

**为什么最小化窗口不能捕？**  
许多窗口在最小化后不会继续渲染内容，WGC 也无法获取有效帧，因此项目选择直接拒绝捕获。

### 6.2 获取 Activation Factory

```cpp
    auto activation_factory = get_activation_factory<GraphicsCaptureItem>();
```

WinRT 中很多类不是直接 new 出来的，而是通过 **Activation Factory（激活工厂）** 创建。`GraphicsCaptureItem` 就属于这种情况。

### 6.3 通过 HWND 创建 GraphicsCaptureItem

```cpp
    // 先转成 IUnknown
    com_ptr<IUnknown> unknownFactory = activation_factory.as<IUnknown>();

    // 再 QueryInterface 到我们手动定义的 Interop 接口
    com_ptr<IGraphicsCaptureItemInterop_Local> interopFactory;
    HRESULT hr = unknownFactory->QueryInterface(
        IID_IGraphicsCaptureItemInterop_Local,
        interopFactory.put_void());

    if (FAILED(hr) || !interopFactory) return false;

    // 用 HWND 创建捕获目标对象
    com_ptr<IInspectable> inspectableItem;
    hr = interopFactory->CreateForWindow(
        hwnd,
        IID_IGraphicsCaptureItem_Local,
        inspectableItem.put_void());

    if (FAILED(hr)) return false;

    // 最后把 IInspectable 转成高层 WinRT 类型
    m_item = inspectableItem.as<GraphicsCaptureItem>();
```

### 6.4 为什么不能直接 `GraphicsCaptureItem::CreateForWindow(hwnd)`？

因为公开的 C++/WinRT API 并没有直接暴露这个静态方法，实际上它是通过 `IGraphicsCaptureItemInterop` 这个底层接口实现的。也就是说：

```
HWND ──(Interop 接口)──> IInspectable ──(WinRT cast)──> GraphicsCaptureItem
```

这就是 WGC 与普通 WinRT API 的一个不太直观的地方。

---

## 7. 帧池：CreateFreeThreaded 的意义

```cpp
m_framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
    m_winrtDevice,
    winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
    1,
    m_item.Size());
```

参数说明：
- **设备**：WinRT 版本的 D3D 设备
- **像素格式**：`B8G8R8A8UIntNormalized`，和 DXGI 的 `DXGI_FORMAT_B8G8R8A8_UNORM` 对应
- **缓冲区数量**：1
- **初始尺寸**：目标窗口大小

### 7.1 为什么使用 `CreateFreeThreaded`？

WGC 有两种创建帧池的方法：
- `Create(...)`
- `CreateFreeThreaded(...)`

区别在于：
- `Create(...)` 要求在有 Dispatcher 的线程上使用（通常是 UI 线程）
- `CreateFreeThreaded(...)` 不依赖 UI 消息循环，适合后台线程

本项目的视频采集运行在 `DxGiCapturer` 的视频线程，不是 UI 线程，所以必须用 `CreateFreeThreaded`。

---

## 8. FrameArrived 回调：异步接收新帧

### 8.1 注册回调

```cpp
m_frameArrivedToken = m_framePool.FrameArrived(
    [this](Direct3D11CaptureFramePool const& sender,
           winrt::Windows::Foundation::IInspectable const&) {
        auto frame = sender.TryGetNextFrame();
        if (!frame) return;

        auto surface = frame.Surface();  // WinRT Surface 对象
        ...
    });
```

WGC 不是轮询模型，而是**事件驱动模型**：当系统捕获到新帧时，主动回调 `FrameArrived`。

### 8.2 从 Surface 拿到底层 D3D11 纹理

```cpp
        auto surface = frame.Surface();

        // 获取底层 DXGI 接口访问器
        com_ptr<IDirect3DDxgiInterfaceAccess_Local> access;
        com_ptr<IUnknown> unknownSurface = surface.as<IUnknown>();
        HRESULT hr = unknownSurface->QueryInterface(
            IID_IDirect3DDxgiInterfaceAccess_Local,
            access.put_void());

        if (SUCCEEDED(hr) && access) {
            com_ptr<ID3D11Texture2D> tex;
            access->GetInterface(__uuidof(ID3D11Texture2D), tex.put_void());
            ...
        }
```

这一步非常关键：

```
frame.Surface()   ← WinRT 世界
    ↓ QueryInterface(IDirect3DDxgiInterfaceAccess)
底层接口访问器
    ↓ GetInterface(__uuidof(ID3D11Texture2D))
ID3D11Texture2D*  ← 回到 D3D11 世界
```

最终，我们拿到了项目后续渲染所需的标准 `ID3D11Texture2D*`，于是 WGC 路线就能和 DXGI 路线完全统一。

---

## 9. 最新帧缓存：m_lastFrame

### 9.1 在回调中缓存最新帧

```cpp
if (tex) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // 用 WRL::ComPtr 缓存最新的一帧
    m_lastFrame = tex.get();
    m_hasNewFrame = true;

    // 处理窗口尺寸变化
    auto contentSize = frame.ContentSize();
    if (contentSize.Width  != m_item.Size().Width ||
        contentSize.Height != m_item.Size().Height) {
        m_framePool.Recreate(
            m_winrtDevice,
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            1,
            contentSize);
    }
}
```

这里的设计不是把每一帧都排队，而是**只保留最新一帧**。

### 9.2 为什么不用队列？

WGC 窗口采集的目标主要是预览和录制，不需要保留历史帧。只缓存最新帧有两个优点：
- 内存占用低
- 不会因为消费速度慢而积压过时帧

这是一种典型的**实时视频系统设计**：宁可丢旧帧，也要保持低延迟。

---

## 10. acquireNextFrame()：供 DxGiCapturer 主循环读取

```cpp
bool WgcCapturer::acquireNextFrame(ID3D11Texture2D** outTexture) {
    if (!m_hasNewFrame) return false;   // 没有新帧，立即返回

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_lastFrame) {
        m_lastFrame.CopyTo(outTexture);  // AddRef + 输出裸指针
        m_hasNewFrame = false;
        return true;
    }
    return false;
}
```

### 10.1 `CopyTo(outTexture)` 的语义

`m_lastFrame` 是 `WRL::ComPtr<ID3D11Texture2D>`，调用 `CopyTo(outTexture)` 会：
- 给底层 COM 对象增加引用计数（`AddRef`）
- 把裸指针写到 `outTexture`

这意味着调用者（`DxGiCapturer`）拿到这个指针后，必须负责后续的生命周期管理。项目中是这样处理的：

```cpp
// DxGiCapturer.cpp
ID3D11Texture2D* wgcTex = nullptr;
if (m_wgcCapturer->acquireNextFrame(&wgcTex)) {
    // 用 ComPtr::Attach 接管这个裸指针
    m_capturedTexture.Attach(wgcTex);
    ...
}
```

`Attach(wgcTex)` 的意思是：这个裸指针已经有引用计数了，不要再 `AddRef`，直接接管它，在 `m_capturedTexture` 析构时自动 `Release()`。

---

## 11. stopCapture()：正确关闭会话

```cpp
void WgcCapturer::stopCapture() {
    if (m_session) {
        m_session.Close();
        m_session = nullptr;
    }
    if (m_framePool) {
        m_framePool.FrameArrived(m_frameArrivedToken);  // 注销回调
        m_framePool.Close();
        m_framePool = nullptr;
    }
    m_item = nullptr;
    m_lastFrame.Reset();
}
```

正确关闭顺序：
1. 先关闭 `GraphicsCaptureSession`
2. 再注销 `FrameArrived` 回调
3. 再关闭 `FramePool`
4. 清理缓存纹理

如果不注销回调，`FramePool` 关闭后仍可能收到悬空回调，导致访问已销毁对象。

---

## 12. WGC 在 DxGiCapturer 中的整合流程

当用户选择一个窗口作为采集源时，`DxGiCapturer::startCapture()` 会走 WGC 路线：

```cpp
else if (isWindow && m_wgcCapturer) {
    ID3D11Texture2D* wgcTex = nullptr;
    if (m_wgcCapturer->acquireNextFrame(&wgcTex)) {
        if (wgcTex) {
            D3D11_TEXTURE2D_DESC d;
            wgcTex->GetDesc(&d);
            m_width  = d.Width;
            m_height = d.Height;

            // 接管纹理
            m_capturedTexture.Attach(wgcTex);

            // 创建 SRV，后续统一走 GPU 合成管线
            m_cachedSRV.Reset();
            m_d3dDevice->CreateShaderResourceView(
                m_capturedTexture.Get(), nullptr, &m_cachedSRV);
            captureSuccess = true;
        }
    } else {
        QThread::msleep(1);  // 没有新帧，短暂休眠
    }
}
```

可以看到，WGC 路线的结果最终也变成了：
- `m_capturedTexture`：当前捕获纹理
- `m_cachedSRV`：对应的 Shader Resource View

之后就和 DXGI 一样，进入 `renderTexture()` → GPU 合成 → Staging 回读 → QImage → FFmpeg 的统一流程。

---

## 13. WGC 与 DXGI 的核心差异总结

| 维度 | DXGI Output Duplication | WGC |
|------|-------------------------|-----|
| 捕获目标 | 显示器/桌面 | 窗口/显示器（本项目主要用于窗口） |
| 接口风格 | 传统 COM / DXGI | WinRT + Interop |
| 获取方式 | 轮询 `AcquireNextFrame()` | 回调 `FrameArrived` + 拉取最新帧 |
| 数据类型 | 直接 `ID3D11Texture2D` | 先 WinRT Surface，再转为 `ID3D11Texture2D` |
| 适合场景 | 整屏录制 | 单窗口录制 |
| 对最小化窗口 | 通常可获取桌面上可见内容 | 最小化窗口无法有效捕获 |
| API 复杂度 | 中等 | 高（涉及 WinRT、Interop、GUID） |

### 13.1 为什么项目不直接全部改成 WGC？

原因主要有三个：
1. **DXGI 捕整屏更成熟、性能更稳定**
2. **WGC 主要优势在窗口捕获，不必强行取代 DXGI**
3. **双路线能最大程度覆盖兼容性问题**

这是一个典型的工程取舍：**不是所有地方都用“新 API”，而是针对场景选最合适的 API。**

---

## 14. 本课作业

1. **流程图练习**：画出 `HWND → GraphicsCaptureItem → FramePool → Surface → ID3D11Texture2D → SRV` 的完整数据流图，并标注每一步所属的技术体系（WinRT / COM / D3D11）。

2. **接口桥接思考**：为什么 `frame.Surface()` 不能直接传给 `renderTexture()`？请解释 `IDirect3DDxgiInterfaceAccess` 在这里扮演的角色。

3. **最小化窗口实验**：运行程序，选择一个窗口作为采集源，然后把该窗口最小化。观察项目的行为，并结合 `WgcCapturer::startCapture()` 中的 `IsIconic(hwnd)` 判断，分析为什么作者选择直接拒绝最小化窗口。

4. **扩展设计题**：如果要让 `WgcCapturer` 也支持**显示器捕获**，理论上可以使用 `IGraphicsCaptureItemInterop::CreateForMonitor(HMONITOR, ...)`。请写出设计方案：
   - 新增哪个方法？
   - `DxGiCapturer` 里如何切换？
   - 与当前 DXGI 路线相比，这样做的优缺点是什么？

---

*上一课：[第5课：DXGI 屏幕采集](./第5课：DXGI屏幕采集.md)*  
*下一课：[第7课：WinRT 摄像头采集](./第7课：WinRT摄像头采集.md)*
