# 第5课：DXGI 屏幕采集

> **目标**：深入理解用 Direct3D 11 + DXGI 捕获桌面画面的完整技术链路，掌握从 GPU 纹理到 `QImage` 的数据路径，理解 GPU 合成渲染与叠加图层的实现原理。  
> **对应文件**：`DxGiCapturer.h`、`DxGiCapturer.cpp`

---

## 1. 为什么屏幕采集需要 GPU？

早期屏幕录制软件（如基于 GDI 的方案）直接在 CPU 上读取屏幕像素，有以下问题：

| 问题 | 原因 |
|------|------|
| CPU 占用高 | 每帧都要在 CPU 上拷贝几 MB 数据 |
| 无法捕获 3D 应用 | DirectX/OpenGL 游戏内容不走 GDI |
| 无法捕获受保护内容 | GDI 被 DRM 屏蔽 |
| 延迟高 | CPU 与 GPU 之间数据传输是瓶颈 |

Windows 8+ 引入的 **DXGI Output Duplication（桌面复制）** 技术解决了这些问题：

- 屏幕内容本来就以 GPU 纹理形式存在于 VRAM
- DXGI 直接把这块纹理共享给应用，**零拷贝获取桌面帧**
- 支持所有 DirectX 应用的内容
- CPU 只在最后"读回"时介入（通过 Staging Texture）

---

## 2. 核心数据结构

### 2.1 Vertex — 顶点结构

```cpp
// DxGiCapturer.cpp
struct Vertex {
    float x, y, z;   // 位置坐标（NDC 空间，范围 -1.0 ~ 1.0）
    float u, v;       // 纹理坐标（范围 0.0 ~ 1.0）
};
```

GPU 渲染的基本单元是三角形，每个顶点有位置和纹理坐标。一个矩形由两个三角形（6个顶点）组成：

```
(-1,1)──────(1,1)       顶点顺序（两个三角形）：
  │  ╲  ▲  │           三角形1：左上→右上→左下
  │  ╲ │  │           三角形2：左下→右上→右下
  │   ╲│  │
(-1,-1)─────(1,-1)
```

### 2.2 HLSL Shader — 内嵌着色器

```cpp
// DxGiCapturer.cpp — 以字符串形式内嵌的 HLSL 代码
const char* shaderCode =
    "Texture2D shaderTexture : register(t0);"        // 纹理输入槽
    "SamplerState sampleType : register(s0);"        // 采样器

    // 顶点着色器：直接透传坐标，不做变换
    "struct VOut { float4 position : SV_POSITION; float2 tex : TEXCOORD0; };"
    "VOut VShader(float4 position : POSITION, float2 tex : TEXCOORD0) {"
    "    VOut output;"
    "    output.position = position;"
    "    output.tex = tex;"
    "    return output;"
    "}"

    // 像素着色器：从纹理采样颜色
    "float4 PShader(VOut input) : SV_TARGET {"
    "    return shaderTexture.Sample(sampleType, input.tex);"
    "}";
```

这是一个最简单的"纹理贴图"着色器：
- **顶点着色器（VShader）**：接收顶点位置和纹理坐标，原样输出（无 3D 变换）
- **像素着色器（PShader）**：对每个像素，从纹理中采样对应位置的颜色

### 2.3 OverlayData — 图层数据

```cpp
// DxGiCapturer.h
struct OverlayData {
    int id = 0;
    OverlayType type = OverlayType::Image;  // Image 或 Camera
    QImage image;           // 图片/文字图层的像素数据（CPU 端）
    WinRTCamera* camera;    // 摄像头图层的数据源
    QRect rect;             // 图层在屏幕上的位置和大小
    bool textureDirty = false;          // 是否需要重新上传到 GPU
    ID3D11Texture2D* texture = nullptr; // GPU 端纹理
    ID3D11ShaderResourceView* srv = nullptr; // 纹理的 SRV（着色器读取视图）
};
```

每个叠加图层（图片、文字、摄像头）都对应一个 `OverlayData`，核心是 CPU 端的 `QImage` 和 GPU 端的 `texture + srv`。

---

## 3. 初始化流程

### 3.1 initDevice — 创建 D3D11 设备

```cpp
bool DxGiCapturer::initDevice(IDXGIAdapter* specificAdapter) {
    // 如果设备已存在且是同一个显卡，直接复用
    if (m_d3dDevice) {
        if (!specificAdapter) return true;
        // 比较 LUID（显卡唯一标识符）
        if (m_currentAdapterLuid == desc.AdapterLuid) return true;
    }

    // 创建 D3D11 设备
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0
    };

    HRESULT hr = D3D11CreateDevice(
        specificAdapter,                          // 指定显卡（null=默认）
        specificAdapter ? D3D_DRIVER_TYPE_UNKNOWN
                        : D3D_DRIVER_TYPE_HARDWARE, // 硬件驱动
        nullptr,                                  // 软件光栅器（不用）
        D3D11_CREATE_DEVICE_BGRA_SUPPORT          // 支持 BGRA 格式（WinRT 需要）
            | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,  // 支持视频处理
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        m_d3dDevice.ReleaseAndGetAddressOf(),     // 输出：设备
        &featureLevel,
        m_d3dContext.ReleaseAndGetAddressOf()     // 输出：上下文
    );

    // 如果硬件设备失败，回退到 WARP（软件模拟）
    if (FAILED(hr) && !specificAdapter)
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, ...);

    if (SUCCEEDED(hr)) {
        // 启用多线程保护（视频线程 + WinRT 摄像头线程同时访问 D3D）
        ComPtr<ID3D11Multithread_Local> multi;
        m_d3dDevice->QueryInterface(IID_ID3D11Multithread_Local, &multi);
        multi->SetMultithreadProtected(TRUE);  // 关键！

        return initShaders();
    }
    return false;
}
```

**重要细节：`SetMultithreadProtected(TRUE)`**

D3D11 设备上下文（`ID3D11DeviceContext`）默认是**非线程安全**的。项目中视频线程在渲染，WinRT 摄像头也在更新纹理，如果不开启多线程保护，会发生竞态条件导致崩溃。

### 3.2 initShaders — 编译着色器与创建 GPU 资源

```cpp
bool DxGiCapturer::initShaders() {
    // 动态加载 d3dcompiler_47.dll（运行时编译 HLSL）
    HMODULE hCompiler = LoadLibraryA("d3dcompiler_47.dll");
    pD3DCompile fnD3DCompile = (pD3DCompile)GetProcAddress(hCompiler, "D3DCompile");

    // 编译顶点着色器（vs_4_0 = Shader Model 4.0）
    ID3DBlob* vsBlob = nullptr;
    fnD3DCompile(shaderCode, strlen(shaderCode), nullptr, nullptr, nullptr,
                 "VShader", "vs_4_0", 0, 0, &vsBlob, &errorBlob);
    m_d3dDevice->CreateVertexShader(vsBlob->GetBufferPointer(),
                                    vsBlob->GetBufferSize(), nullptr, &m_vertexShader);

    // 定义顶点输入布局（告诉 GPU 顶点数据的格式）
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        // POSITION：xyz，每个 float 4 字节，偏移 0
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0},
        // TEXCOORD：uv，偏移 12（xyz 各 4 字节 = 12 字节）
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}
    };
    m_d3dDevice->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(),
                                   vsBlob->GetBufferSize(), &m_inputLayout);

    // 编译像素着色器
    fnD3DCompile(..., "PShader", "ps_4_0", ...);
    m_d3dDevice->CreatePixelShader(..., &m_pixelShader);

    // 创建顶点缓冲区（动态，CPU 可写，用于每帧更新顶点位置）
    D3D11_BUFFER_DESC vbDesc = {
        sizeof(Vertex) * 6,         // 6 个顶点（两个三角形）
        D3D11_USAGE_DYNAMIC,        // CPU 可写，GPU 只读
        D3D11_BIND_VERTEX_BUFFER,
        D3D11_CPU_ACCESS_WRITE
    };
    m_d3dDevice->CreateBuffer(&vbDesc, nullptr, &m_vertexBuffer);

    // 采样器状态（双线性过滤，边缘钳制）
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;  // 双线性过滤（平滑缩放）
    sampDesc.AddressU = sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    m_d3dDevice->CreateSamplerState(&sampDesc, &m_samplerState);

    // Alpha 混合状态（源颜色 * Alpha + 目标颜色 * (1-Alpha)）
    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend  = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp   = D3D11_BLEND_OP_ADD;
    m_d3dDevice->CreateBlendState(&blendDesc, &m_blendState);

    return true;
}
```

**Alpha 混合是叠加图层透明度的关键**：摄像头画面叠加到桌面上时，如果摄像头帧有透明区域（Alpha < 1），就需要混合底层桌面颜色。公式：`输出 = 源.rgb * 源.a + 目标.rgb * (1 - 源.a)`

### 3.3 initDuplication — 创建桌面复制接口

```cpp
bool DxGiCapturer::initDuplication(int monitorIndex, ComPtr<IDXGIOutputDuplication>& outDupl) {
    // 枚举所有显卡（Adapter）和显示器（Output）
    ComPtr<IDXGIFactory1> factory;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);

    int currentGlobalIndex = 0;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT ai = 0; factory->EnumAdapters1(ai, &adapter) != DXGI_ERROR_NOT_FOUND; ai++) {
        ComPtr<IDXGIOutput> output;
        for (UINT oi = 0; adapter->EnumOutputs(oi, &output) != DXGI_ERROR_NOT_FOUND; oi++) {
            if (currentGlobalIndex == monitorIndex) {
                // 找到目标显示器 —— 确保 D3D 设备在此显卡上创建
                ComPtr<IDXGIAdapter> targetAdapter;
                adapter.As(&targetAdapter);
                initDevice(targetAdapter.Get());  // 重要：显卡切换时重新创建设备

                // 将 IDXGIOutput 升级到 IDXGIOutput1（支持桌面复制）
                ComPtr<IDXGIOutput1> output1;
                output.As(&output1);

                // 创建桌面复制接口
                return SUCCEEDED(output1->DuplicateOutput(m_d3dDevice.Get(), &outDupl));
            }
            currentGlobalIndex++;
        }
    }
    return false;
}
```

**多显卡注意事项**：如果用户有两块独立显卡，每块显卡只能复制连接到它的显示器。切换到另一显卡的显示器时，必须在对应显卡上重新创建 D3D 设备。

---

## 4. 采集主循环：startCapture()

这是整个模块的核心，约 280 行代码。让我们分段理解：

### 4.1 帧率控制初始化

```cpp
void DxGiCapturer::startCapture() {
    m_running = true;
    emit initFinished(true);

    const double TARGET_FPS = 30.0;
    const double TARGET_FRAME_TIME_SEC = 1.0 / 30.0;  // ≈ 33.3ms

    QElapsedTimer loopTimer;
    loopTimer.start();
    double nextFrameTargetTime = loopTimer.nsecsElapsed() / 1e9;
    // ...
```

### 4.2 采集源切换检测

```cpp
    while (m_running) {
        QCoreApplication::processEvents();  // 处理 Qt 事件（invokeMethod 投递的调用）

        // 检测切换采集源的请求（显示器/窗口）
        bool needSwitch = false;
        {
            QMutexLocker locker(&m_targetMutex);
            if (m_targetChanged) {
                m_currentTarget = m_pendingTarget;
                m_targetChanged = false;
                needSwitch = true;
            }
        }
        if (needSwitch)
            resetCaptureState();  // 释放旧的 DXGI 复制接口，重新初始化
```

### 4.3 DXGI 桌面复制 — 获取 GPU 帧

```cpp
        if (isMonitor && m_deskDupl) {
            // 等待下一帧桌面内容（最多等 100ms）
            HRESULT hr = m_deskDupl->AcquireNextFrame(
                100,              // 超时毫秒（桌面静止时会一直等）
                &frameInfo,       // 帧信息（时间戳、指针位置等）
                &desktopResource  // 输出：GPU 资源接口
            );

            if (SUCCEEDED(hr)) {
                // 将 IDXGIResource 转换为 ID3D11Texture2D
                ComPtr<ID3D11Texture2D> rawTex;
                desktopResource.As(&rawTex);

                // 获取纹理描述（宽高、格式）
                D3D11_TEXTURE2D_DESC desc;
                rawTex->GetDesc(&desc);
                m_width  = desc.Width;
                m_height = desc.Height;

                // 复制到我们自己的纹理（AcquireNextFrame 返回的纹理由 DXGI 管理，
                // ReleaseFrame 后就失效了，必须先拷贝）
                m_d3dContext->CopyResource(m_currentMonitorTexture.Get(), rawTex.Get());

                m_deskDupl->ReleaseFrame();  // 释放帧（必须调用！否则 DXGI 缓冲区耗尽）
                captureSuccess = true;

            } else if (hr == DXGI_ERROR_ACCESS_LOST) {
                // UAC 弹窗、锁屏等"安全桌面"切换时会触发
                accessLost = true;
            }
        }

        if (accessLost) {
            resetCaptureState();    // 销毁旧的复制接口
            QThread::msleep(100);   // 等待安全桌面切换完成
            continue;               // 下次循环重新 initDuplication
        }
```

**`DXGI_ERROR_ACCESS_LOST` 是项目早期崩溃的根源之一！**

当 Windows 切换到"安全桌面"（UAC 权限弹窗、锁屏、Ctrl+Alt+Del 菜单等）时，普通进程的 DXGI 复制接口会失效，`AcquireNextFrame` 返回 `DXGI_ERROR_ACCESS_LOST`。早期代码没有处理这个错误，导致崩溃。现在的修复方案是：捕获错误 → 重置接口 → 等待 → 重新初始化。

---

## 5. GPU 合成渲染管线

获取到桌面帧后，需要在 GPU 上把桌面、图片图层、摄像头图层合成为一帧输出。

### 5.1 四种关键纹理的角色

```
┌─────────────────────────────────────────────────────────────────┐
│                       纹理资源一览                               │
├─────────────────┬───────────────────────────────────────────────┤
│ m_currentMonitor│ DXGI 采集的桌面帧（GPU→GPU 拷贝）             │
│ Texture         │ BindFlags: SHADER_RESOURCE                    │
├─────────────────┼───────────────────────────────────────────────┤
│ m_composition   │ 合成画布，所有图层都渲染到这里                 │
│ Texture         │ BindFlags: RENDER_TARGET | SHADER_RESOURCE    │
├─────────────────┼───────────────────────────────────────────────┤
│ m_stagingTexture│ 暂存纹理，用于 GPU→CPU 数据回读               │
│                 │ Usage: STAGING，CPUAccessFlags: READ           │
├─────────────────┼───────────────────────────────────────────────┤
│ OverlayData     │ 每个图层的 GPU 纹理（CPU 图片上传而来）        │
│ .texture/.srv   │ Usage: DYNAMIC，CPUAccessFlags: WRITE         │
└─────────────────┴───────────────────────────────────────────────┘
```

### 5.2 创建合成纹理和 Staging 纹理

```cpp
// 合成纹理（渲染目标）
D3D11_TEXTURE2D_DESC compDesc = {};
compDesc.Width   = renderW;
compDesc.Height  = renderH;
compDesc.Format  = DXGI_FORMAT_B8G8R8A8_UNORM;
compDesc.Usage   = D3D11_USAGE_DEFAULT;
compDesc.BindFlags = D3D11_BIND_RENDER_TARGET      // 可以作为渲染目标
                   | D3D11_BIND_SHADER_RESOURCE;   // 可以被着色器读取
m_d3dDevice->CreateTexture2D(&compDesc, nullptr, &m_compositionTexture);
m_d3dDevice->CreateRenderTargetView(m_compositionTexture.Get(), nullptr, &m_compositionRTV);

// Staging 纹理（CPU 读取桥梁）
D3D11_TEXTURE2D_DESC stageDesc = compDesc;
stageDesc.Usage        = D3D11_USAGE_STAGING;  // 专门用于 CPU 读取
stageDesc.BindFlags    = 0;                    // 不绑定到渲染管线
stageDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; // CPU 可以读取
m_d3dDevice->CreateTexture2D(&stageDesc, nullptr, &m_stagingTexture);
```

### 5.3 渲染管线设置

```cpp
// 清空合成画布（黑色背景）
float clearColor[] = {0, 0, 0, 1};
m_d3dContext->ClearRenderTargetView(m_compositionRTV.Get(), clearColor);

// 将合成纹理设为渲染目标
m_d3dContext->OMSetRenderTargets(1, m_compositionRTV.GetAddressOf(), nullptr);

// 设置光栅化状态（双面渲染，不剔除背面）
m_d3dContext->RSSetState(m_rasterizerState.Get());

// 设置顶点输入布局
m_d3dContext->IASetInputLayout(m_inputLayout.Get());
m_d3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

// 绑定着色器
m_d3dContext->VSSetShader(m_vertexShader.Get(), nullptr, 0);
m_d3dContext->PSSetShader(m_pixelShader.Get(), nullptr, 0);
m_d3dContext->PSSetSamplers(0, 1, m_samplerState.GetAddressOf());

// 开启 Alpha 混合
m_d3dContext->OMSetBlendState(m_blendState.Get(), blendFactor, 0xffffffff);
```

### 5.4 renderTexture — 绘制一个矩形

```cpp
void DxGiCapturer::renderTexture(ID3D11ShaderResourceView* srv,
                                  const QRect& rect,
                                  bool isBackground) {
    float left, right, top, bottom;

    if (isBackground) {
        // 背景全屏：NDC 坐标范围 -1 ~ +1
        left = -1.0f; right = 1.0f; top = 1.0f; bottom = -1.0f;
    } else {
        // 叠加图层：将像素坐标转换为 NDC 坐标
        // NDC_x = pixel_x / width * 2 - 1
        float w = (float)m_width, h = (float)m_height;
        left   = rect.x()                  / w * 2.0f - 1.0f;
        right  = (rect.x() + rect.width()) / w * 2.0f - 1.0f;
        top    = 1.0f - rect.y()                  / h * 2.0f;
        bottom = 1.0f - (rect.y() + rect.height())/ h * 2.0f;
    }

    // 构建 6 个顶点（两个三角形）
    Vertex vertices[] = {
        {left,  top,    0, 0, 0},  // 左上，纹理坐标(0,0)
        {right, top,    0, 1, 0},  // 右上，纹理坐标(1,0)
        {left,  bottom, 0, 0, 1},  // 左下，纹理坐标(0,1)
        {left,  bottom, 0, 0, 1},  // 左下（第二个三角形起点）
        {right, top,    0, 1, 0},  // 右上
        {right, bottom, 0, 1, 1},  // 右下，纹理坐标(1,1)
    };

    // 更新顶点缓冲区（CPU → GPU）
    D3D11_MAPPED_SUBRESOURCE map;
    m_d3dContext->Map(m_vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
    memcpy(map.pData, vertices, sizeof(vertices));
    m_d3dContext->Unmap(m_vertexBuffer.Get(), 0);

    // 绑定顶点缓冲区并绘制
    UINT stride = sizeof(Vertex), offset = 0;
    m_d3dContext->IASetVertexBuffers(0, 1, m_vertexBuffer.GetAddressOf(), &stride, &offset);
    m_d3dContext->PSSetShaderResources(0, 1, &srv);  // 绑定纹理
    m_d3dContext->Draw(6, 0);                         // 绘制 6 个顶点
}
```

### 5.5 按 Z-Order 合成所有图层

```cpp
// 第1层：桌面背景（全屏）
if (m_cachedSRV)
    renderTexture(m_cachedSRV.Get(), QRect(0,0,m_width,m_height), true);

// 第2步：处理待更新（图层的 CPU→GPU 纹理上传、删除清理）
processPendingUpdates();

// 第3层：按 Z-Order 绘制所有叠加图层（从下到上）
QMutexLocker locker(&m_dataMutex);
for (int id : m_renderOrder) {         // m_renderOrder 决定绘制顺序
    OverlayData* data = m_overlays[id];
    if (data->rect.isEmpty()) continue;

    if (data->type == OverlayType::Image && data->srv) {
        // 图片/文字图层
        renderTexture(data->srv, data->rect, false);
    } else if (data->type == OverlayType::Camera && data->camera) {
        // 摄像头图层 —— SRV 由 WinRTCamera 实时更新
        auto camSRV = data->camera->getLatestFrame();
        if (camSRV)
            renderTexture(camSRV.Get(), data->rect, false);
    }
}
```

---

## 6. Staging Texture：GPU → CPU 的数据回读

合成完成后，需要把 GPU 上的结果读回 CPU，才能转成 `QImage` 送给 FFmpeg 编码：

```cpp
// 第1步：从合成纹理拷贝到 Staging 纹理（GPU 内部，非常快）
m_d3dContext->CopyResource(m_stagingTexture.Get(), m_compositionTexture.Get());
m_d3dContext->Flush();  // 确保 GPU 命令全部执行完毕

// 第2步：Map Staging 纹理（阻塞，等待 GPU 完成）
D3D11_MAPPED_SUBRESOURCE mapInfo;
m_d3dContext->Map(m_stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapInfo);

// 第3步：从 mapInfo.pData 构建 QImage（深拷贝！）
QImage deepCopy;
if (mapInfo.RowPitch == (UINT)m_width * 4) {
    // 行步长匹配：直接整块拷贝
    deepCopy = QImage((uchar*)mapInfo.pData, m_width, m_height,
                       QImage::Format_ARGB32_Premultiplied).copy();
} else {
    // 行步长不匹配（GPU 有对齐填充）：逐行拷贝
    deepCopy = QImage(m_width, m_height, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < m_height; ++y)
        memcpy(deepCopy.bits() + y * m_width * 4,
               (uchar*)mapInfo.pData + y * mapInfo.RowPitch,
               m_width * 4);
}
m_d3dContext->Unmap(m_stagingTexture.Get(), 0);

// 第4步：根据需要分发
if (m_isRecordingMP4)
    emit frameCaptured(deepCopy, timestamp);    // 送给编码器

if (m_previewEnabled && m_uiInFlight == 0) {
    m_uiInFlight = 1;
    emit frameCapturedForPreview(deepCopy);     // 送给 UI 预览
}

if (m_snapshotRequested.exchange(0) == 1)
    emit snapshotCaptured(deepCopy);            // 截图
```

**为什么需要 Staging Texture？**

```
直接读 compositionTexture：
  compositionTexture.BindFlags = RENDER_TARGET | SHADER_RESOURCE
  → 无 CPU_ACCESS_READ 权限
  → 无法 Map 读取
  → 编译/运行报错

通过 Staging Texture 中转：
  compositionTexture（GPU 专用）
      ↓ CopyResource（GPU 内部，极快）
  stagingTexture（CPU 可读）
      ↓ Map（同步等待 GPU 完成）
  CPU 内存 → QImage
```

**RowPitch 的坑**：GPU 出于性能对齐的考虑，每行数据后面可能有填充字节。例如 `m_width = 1920`，理论上每行 `1920 × 4 = 7680` 字节，但 `RowPitch` 可能是 `7936`（对齐到 256 字节边界）。如果直接整块 `memcpy` 不考虑 `RowPitch`，图像会出现"斜切"错位现象。

---

## 7. 精确帧率控制：preciseSleep

```cpp
void DxGiCapturer::preciseSleep(double targetSeconds) {
    if (targetSeconds <= 0) return;
    double targetMs = targetSeconds * 1000.0;
    QElapsedTimer t;
    t.start();
    // 如果需要等待超过 2ms，先用系统睡眠粗略等待（节省 CPU）
    if (targetMs > 2.0)
        QThread::msleep((unsigned long)(targetMs - 1.0));
    // 最后 1ms 用忙等精确补偿（系统睡眠精度约 ±1ms）
    while (t.nsecsElapsed() / 1e9 < targetSeconds) { }
}

// 使用方式（主循环末尾）：
nextFrameTargetTime += TARGET_FRAME_TIME_SEC;  // 下一帧的目标时间
double now = loopTimer.nsecsElapsed() / 1e9;
double sleepTime = nextFrameTargetTime - now;
if (sleepTime > 0)
    preciseSleep(sleepTime);
else
    nextFrameTargetTime = now;  // 如果本帧处理超时，跳过等待，立即处理下一帧
```

**为什么不直接用 `QThread::msleep(33)`？**

`msleep(33)` 的实际精度约为 ±10ms（Windows 系统定时器精度约 15ms）。如果每帧多睡 5ms，30fps × 5ms = 150ms 的累积误差，导致录制视频实际帧率只有 ~25fps，时长与实时不符。

`preciseSleep` 用"粗睡眠 + 忙等"的混合策略，将精度提升到亚毫秒级，代价是少量 CPU 占用。

---

## 8. 图层管理：CPU 图像 → GPU 纹理

### 8.1 updateTextureFromImage — 上传到 GPU

```cpp
void DxGiCapturer::updateTextureFromImage(OverlayData* data) {
    // 转为 BGRA8 格式（DirectX 标准格式）
    QImage converted = data->image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    int w = converted.width(), h = converted.height();

    // 如果尺寸变化，销毁旧纹理
    if (data->texture) {
        D3D11_TEXTURE2D_DESC d;
        data->texture->GetDesc(&d);
        if (d.Width != (UINT)w || d.Height != (UINT)h) {
            SAFE_RELEASE(data->texture);
            SAFE_RELEASE(data->srv);
        }
    }

    // 创建动态纹理（CPU 可写）
    if (!data->texture) {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width  = w; desc.Height = h;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Usage  = D3D11_USAGE_DYNAMIC;         // CPU 可频繁写入
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        m_d3dDevice->CreateTexture2D(&desc, nullptr, &data->texture);
        m_d3dDevice->CreateShaderResourceView(data->texture, nullptr, &data->srv);
    }

    // 写入像素数据（Map → memcpy → Unmap）
    D3D11_MAPPED_SUBRESOURCE map;
    m_d3dContext->Map(data->texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
    // 处理行步长不匹配
    if (converted.bytesPerLine() == (int)map.RowPitch) {
        memcpy(map.pData, converted.constBits(), h * converted.bytesPerLine());
    } else {
        for (int y = 0; y < h; ++y)
            memcpy((uchar*)map.pData + y * map.RowPitch,
                   converted.constBits() + y * converted.bytesPerLine(), w * 4);
    }
    m_d3dContext->Unmap(data->texture, 0);
    data->textureDirty = false;
}
```

### 8.2 textureDirty 延迟上传机制

图层的 CPU 数据更新（如用户移动图片、摄像头帧更新）与 GPU 纹理上传是**分离的**：

```
UI 线程更新图层图片：
    m_overlays[id]->image = newImage   ← 仅修改 CPU 端数据
    m_overlays[id]->textureDirty = true  ← 标记需要上传

视频线程每帧：
    processPendingUpdates()
        if (data->textureDirty)
            updateTextureFromImage(data)  ← 真正上传到 GPU
```

这样可以避免高频的图层更新（如 30fps 的摄像头帧）每次都立即触发 GPU 上传，而是在渲染循环的统一时机批量处理。

---

## 9. 窗口枚举：getWindowList

```cpp
// 枚举所有可见窗口的回调函数
BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* map = reinterpret_cast<QMap<size_t, QString>*>(lParam);

    if (IsWindowVisible(hwnd) && GetWindowTextLength(hwnd) > 0) {
        // 过滤 Cloaked 窗口（如 UWP 应用在后台时）
        int cloaked = 0;
        DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked) return TRUE;

        RECT rect;
        GetWindowRect(hwnd, &rect);
        if ((rect.right - rect.left) > 0 && (rect.bottom - rect.top) > 0) {
            TCHAR title[256];
            GetWindowText(hwnd, title, 256);
            QString str = QString::fromWCharArray(title);
            // 过滤系统内置窗口（避免混乱）
            if (str != "Program Manager" && str != "Settings" ...)
                map->insert((size_t)hwnd, str);  // HWND 作为 key
        }
    }
    return TRUE;
}

QMap<size_t, QString> DxGiCapturer::getWindowList() {
    QMap<size_t, QString> list;
    EnumWindows(EnumWindowsProc, (LPARAM)&list);  // 传递 map 指针
    return list;
}
```

`HWND` 被存储为 `size_t`，因为 Qt 的 `QVariant` 无法直接存储 `HWND`（它是 Windows 平台特有类型）。通过 `(size_t)hwnd` 转换，可以安全地在 `QVariant` 中传递，使用时再转回 `(HWND)id`。

---

## 10. 整体数据流总结

```
                        ┌── 显示器采集 ──────────────────────────────┐
                        │                                            │
                        │  DXGI::AcquireNextFrame()                 │
                        │       ↓                                   │
                        │  rawTex (DXGI 管理)                       │
                        │       ↓ CopyResource                      │
                        │  m_currentMonitorTexture (我们管理)        │
                        │       ↓ CreateSRV                         │
                        │  m_cachedSRV ──────────────────────┐      │
                        └────────────────────────────────────│──────┘
                                                             ↓
┌── 图层采集 ─────────────────────────────────────────────  renderTexture(背景)
│                                                             ↓
│  QImage → Map/memcpy → OverlayData.texture                renderTexture(图层1)
│  WinRTCamera.getLatestFrame() → SRV                        ↓
│                                                            renderTexture(图层N)
└─────────────────────────────────────────────────────────────↓
                                                   m_compositionTexture（合成结果）
                                                             ↓ CopyResource
                                                   m_stagingTexture（可被CPU读）
                                                             ↓ Map/memcpy
                                                   QImage（CPU 内存）
                                                        ↙         ↘
                                            emit frameCaptured  emit frameCapturedForPreview
                                                   ↓                    ↓
                                           FFmpegRecorder           MainWindow
                                           (编码→MP4/RTMP)         (UI预览)
```

---

## 11. 本课作业

1. **RowPitch 实验**：在 `startCapture()` 的 Map 结果处，加一行 `qDebug() << "RowPitch:" << mapInfo.RowPitch << "expected:" << m_width * 4;`，运行程序，观察两个值是否相等。如果不等，GPU 在行末填充了多少字节？

2. **坐标转换推导**：`renderTexture` 中将像素坐标转为 NDC 的公式是：
   ```
   NDC_x = pixel_x / width * 2.0f - 1.0f
   ```
   请手动验证：当 `pixel_x = 0` 时结果是 `-1`（左边缘），当 `pixel_x = width` 时结果是 `+1`（右边缘）。Y 轴为什么公式要用 `1.0f - ...`？

3. **添加新图层类型**：目前 `OverlayType` 只有 `Image` 和 `Camera`。如果要添加一个**时钟图层**（每秒更新一次，显示当前时间），需要修改哪些文件的哪些部分？写出设计方案（不需要完整实现）。

4. **`preciseSleep` 对比实验**：将 `preciseSleep` 替换为 `QThread::msleep((unsigned long)(sleepTime * 1000))`，录制 10 秒视频，观察：
   - 视频实际时长是否还是 10 秒（用播放器查看）？
   - 帧率是否稳定（用 `ffprobe -show_frames` 分析）？
   - CPU 占用是否下降？

---

*上一课：[第4课：SystemController 与多线程管理](./第4课：SystemController与多线程管理.md)*  
*下一课：[第6课：WGC 窗口采集与 D3D 纹理操作](./第6课：WGC窗口采集与D3D纹理操作.md)*
