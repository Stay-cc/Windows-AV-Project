#include "DxGiCapturer.h"
#include <QDebug>
#include <QThread>
#include <vector>
#include <QCoreApplication>
#include <dwmapi.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

#define SAFE_RELEASE(p) { if(p) { (p)->Release(); (p)=nullptr; } }

// 辅助：获取顶层窗口
HWND GetRootWindow(HWND hwnd) {
    HWND root = GetAncestor(hwnd, GA_ROOT);
    return root ? root : hwnd;
}

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* map = reinterpret_cast<QMap<size_t, QString>*>(lParam);
    if (IsWindowVisible(hwnd) && GetWindowTextLength(hwnd) > 0) {
        int cloaked = 0;
        HRESULT hr = DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (SUCCEEDED(hr) && cloaked) return TRUE;

        RECT rect; GetWindowRect(hwnd, &rect);
        if ((rect.right - rect.left) > 0 && (rect.bottom - rect.top) > 0) {
            TCHAR title[256];
            GetWindowText(hwnd, title, 256);
            QString str = QString::fromWCharArray(title);
            if (str != "Program Manager" && str != "Settings" && str != "Microsoft Text Input Application") {
                map->insert((size_t)hwnd, str);
            }
        }
    }
    return TRUE;
}

QMap<size_t, QString> DxGiCapturer::getWindowList() {
    QMap<size_t, QString> list;
    EnumWindows(EnumWindowsProc, (LPARAM)&list);
    return list;
}

const char* shaderCode = R"(
    Texture2D shaderTexture : register(t0);
    SamplerState sampleType : register(s0);
    struct VOut { float4 position : SV_POSITION; float2 tex : TEXCOORD0; };
    VOut VShader(float4 position : POSITION, float2 tex : TEXCOORD0) {
        VOut output; output.position = position; output.tex = tex; return output;
    }
    float4 PShader(VOut input) : SV_TARGET {
        float4 color = shaderTexture.Sample(sampleType, input.tex);
        return float4(color.r, color.g, color.b, 1.0f);
    }
)";

struct Vertex { float x, y, z; float u, v; };

DxGiCapturer::DxGiCapturer(QObject *parent) : QObject(parent), m_running(0), m_snapshotRequested(0) {}
DxGiCapturer::~DxGiCapturer() { stopCapture(); cleanup(); }

QStringList DxGiCapturer::getMonitorNames() {
    QStringList list;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) return list;
    ComPtr<IDXGIAdapter1> adapter; UINT adapterIndex = 0;
    while (factory->EnumAdapters1(adapterIndex++, &adapter) != DXGI_ERROR_NOT_FOUND) {
        ComPtr<IDXGIOutput> output; UINT outputIndex = 0;
        while (adapter->EnumOutputs(outputIndex++, &output) != DXGI_ERROR_NOT_FOUND) {
            DXGI_OUTPUT_DESC desc; output->GetDesc(&desc);
            list.append(QString("Monitor %1: %2x%3").arg(list.size()).arg(desc.DesktopCoordinates.right - desc.DesktopCoordinates.left).arg(desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top));
        }
    }
    return list;
}

void DxGiCapturer::cleanup() {
    resetCaptureState(); // 清理采集对象

    // 清理渲染资源
    m_stagingTexture.Reset(); m_compositionTexture.Reset(); m_compositionRTV.Reset();
    m_vertexShader.Reset(); m_pixelShader.Reset();
    m_inputLayout.Reset(); m_vertexBuffer.Reset(); m_samplerState.Reset();
    m_blendState.Reset(); m_rasterizerState.Reset(); m_depthStencilState.Reset();

    QMutexLocker locker(&m_dataMutex);
    for(auto data : m_overlays) {
        if(data->texture) data->texture->Release();
        if(data->srv) data->srv->Release();
        delete data;
    }
    m_overlays.clear();
    m_d3dContext.Reset(); m_d3dDevice.Reset();
}

// 新增：专门用于重置采集会话状态，用于 ACCESS_LOST 后恢复
void DxGiCapturer::resetCaptureState() {
    if (m_wgcCapturer) { delete m_wgcCapturer; m_wgcCapturer = nullptr; }
    m_deskDupl.Reset();
    m_currentMonitorTexture.Reset();
    m_cachedSRV.Reset();
    m_capturedTexture.Reset();
}

void DxGiCapturer::stopCapture() { m_running = 0; }

bool DxGiCapturer::initShaders() {
    ID3DBlob* vsBlob = nullptr; ID3DBlob* psBlob = nullptr;
    D3DCompile(shaderCode, strlen(shaderCode), nullptr, nullptr, nullptr, "VShader", "vs_4_0", 0, 0, &vsBlob, nullptr);
    if(!vsBlob) return false;
    m_d3dDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vertexShader);
    D3D11_INPUT_ELEMENT_DESC layout[] = { { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 }, { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
    m_d3dDevice->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_inputLayout);
    vsBlob->Release();
    D3DCompile(shaderCode, strlen(shaderCode), nullptr, nullptr, nullptr, "PShader", "ps_4_0", 0, 0, &psBlob, nullptr);
    if(!psBlob) return false;
    m_d3dDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_pixelShader);
    psBlob->Release();
    D3D11_BUFFER_DESC vbDesc = { sizeof(Vertex) * 6, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
    m_d3dDevice->CreateBuffer(&vbDesc, nullptr, &m_vertexBuffer);
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = sampDesc.AddressV = sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    m_d3dDevice->CreateSamplerState(&sampDesc, &m_samplerState);
    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO; blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = 0x0F;
    m_d3dDevice->CreateBlendState(&blendDesc, &m_blendState);
    D3D11_RASTERIZER_DESC rsDesc = {}; rsDesc.CullMode = D3D11_CULL_NONE; rsDesc.FillMode = D3D11_FILL_SOLID;
    m_d3dDevice->CreateRasterizerState(&rsDesc, &m_rasterizerState);
    D3D11_DEPTH_STENCIL_DESC dsDesc = {}; m_d3dDevice->CreateDepthStencilState(&dsDesc, &m_depthStencilState);
    return true;
}

bool DxGiCapturer::initDevice(IDXGIAdapter* specificAdapter) {
    cleanup();

    // 不再在这里创建 m_wgcCapturer，移到 startCapture 循环中按需创建

    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(specificAdapter, specificAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels, 1, D3D11_SDK_VERSION, &m_d3dDevice, &featureLevel, &m_d3dContext);
    if (SUCCEEDED(hr)) {
        if (!initShaders()) return false;
        return true;
    }
    return false;
}

bool DxGiCapturer::init() { return initDevice(nullptr); }

bool DxGiCapturer::initDuplication(int monitorIndex, ComPtr<IDXGIOutputDuplication>& outDupl) {
    ComPtr<IDXGIDevice> dxgiDevice;
    m_d3dDevice.As(&dxgiDevice);
    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);

    ComPtr<IDXGIOutput> output;
    UINT i = 0;
    while (adapter->EnumOutputs(i++, &output) != DXGI_ERROR_NOT_FOUND) {
        if (i - 1 == monitorIndex) {
            ComPtr<IDXGIOutput1> output1;
            if (SUCCEEDED(output.As(&output1))) {
                return SUCCEEDED(output1->DuplicateOutput(m_d3dDevice.Get(), &outDupl));
            }
        }
    }
    return false;
}

void DxGiCapturer::setCaptureTarget(const CaptureTarget& target) {
    QMutexLocker locker(&m_targetMutex);
    if (m_currentTarget != target) {
        m_pendingTarget = target;
        m_targetChanged = true;
    }
}

// 核心重构：实现自动重连的采集循环
void DxGiCapturer::startCapture() {
    if (m_running) return;
    m_running = 1;

    // 初始设置
    { QMutexLocker locker(&m_targetMutex); m_currentTarget = m_pendingTarget; m_targetChanged = true; }

    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    ComPtr<IDXGIResource> desktopResource;

    while (m_running) {
        QCoreApplication::processEvents();

        // 1. 检查是否需要切换源 (用户请求)
        bool needSwitch = false;
        {
            QMutexLocker locker(&m_targetMutex);
            if (m_targetChanged) {
                m_currentTarget = m_pendingTarget;
                m_targetChanged = false;
                needSwitch = true;
            }
        }

        // 如果用户主动切换，强制重置状态
        if (needSwitch) {
            resetCaptureState();
        }

        // 2. 状态机：检查当前采集对象是否就绪，如果没有则尝试初始化
        bool isMonitor = (m_currentTarget.type == CaptureSourceType::Monitor);
        bool isWindow = (m_currentTarget.type == CaptureSourceType::Window);
        bool initialized = false;

        if (isMonitor) {
            if (!m_deskDupl) {
                // 尝试初始化 DXGI Duplicate
                if (initDuplication(m_currentTarget.monitorIndex, m_deskDupl)) {
                    qDebug() << "Monitor Duplication Initialized.";
                    initialized = true;
                } else {
                    // 初始化失败 (可能处于安全桌面)，等待后重试
                    QThread::msleep(500);
                    continue; // 跳过本次循环，继续尝试
                }
            } else {
                initialized = true;
            }
        }
        else if (isWindow) {
            if (!m_wgcCapturer) {
                if (!IsWindow(m_currentTarget.windowHandle) || IsIconic(m_currentTarget.windowHandle)) {
                    // 窗口无效，发送错误并等待
                    // emit errorOccurred("Window invalid or minimized"); // 可选：太频繁可以不发
                    QThread::msleep(500);
                    continue;
                }

                HWND rootHwnd = GetRootWindow(m_currentTarget.windowHandle);
                m_wgcCapturer = new WgcCapturer();
                m_wgcCapturer->init(m_d3dDevice.Get()); // 每次重建都要 init

                if (m_wgcCapturer->startCapture(rootHwnd)) {
                    qDebug() << "WGC Session Initialized.";
                    initialized = true;
                } else {
                    delete m_wgcCapturer; m_wgcCapturer = nullptr;
                    QThread::msleep(500);
                    continue;
                }
            } else {
                initialized = true;
            }
        }

        if (!initialized) {
            QThread::msleep(100);
            continue;
        }

        // 3. 执行采集 (Capture)
        bool captureSuccess = false;
        bool accessLost = false;

        if (isMonitor && m_deskDupl) {
            HRESULT hr = m_deskDupl->AcquireNextFrame(50, &frameInfo, &desktopResource);
            if (SUCCEEDED(hr)) {
                ComPtr<ID3D11Texture2D> rawTex;
                hr = desktopResource.As(&rawTex);
                desktopResource.Reset();

                if (SUCCEEDED(hr)) {
                    D3D11_TEXTURE2D_DESC desc; rawTex->GetDesc(&desc);
                    // 检查纹理尺寸变化或初始化缓存
                    if (!m_currentMonitorTexture || desc.Width != m_width || desc.Height != m_height) {
                        m_currentMonitorTexture.Reset(); m_cachedSRV.Reset();
                        D3D11_TEXTURE2D_DESC cacheDesc = desc;
                        cacheDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE; cacheDesc.MiscFlags = 0; cacheDesc.CPUAccessFlags = 0; cacheDesc.Usage = D3D11_USAGE_DEFAULT;
                        m_d3dDevice->CreateTexture2D(&cacheDesc, nullptr, &m_currentMonitorTexture);
                        if(m_currentMonitorTexture) m_d3dDevice->CreateShaderResourceView(m_currentMonitorTexture.Get(), nullptr, &m_cachedSRV);
                    }
                    if(m_currentMonitorTexture) m_d3dContext->CopyResource(m_currentMonitorTexture.Get(), rawTex.Get());
                    captureSuccess = true;
                }
                m_deskDupl->ReleaseFrame();
            }
            else if (hr == DXGI_ERROR_ACCESS_LOST) {
                // 【关键逻辑】安全桌面触发 (如任务管理器、UAC)
                qDebug() << "DXGI Access Lost! Resetting...";
                accessLost = true;
            } else if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
                // 超时通常意味着画面静止，不是错误，继续使用上一帧
                captureSuccess = (m_cachedSRV != nullptr);
            }
        }
        else if (isWindow && m_wgcCapturer) {
            ID3D11Texture2D* wgcTex = nullptr;
            if (m_wgcCapturer->acquireNextFrame(&wgcTex)) {
                m_capturedTexture.Attach(wgcTex);
                m_cachedSRV.Reset();
                m_d3dDevice->CreateShaderResourceView(m_capturedTexture.Get(), nullptr, &m_cachedSRV);
                captureSuccess = true;
            } else {
                // WGC 没有新帧，保留上一帧
                captureSuccess = (m_cachedSRV != nullptr);
            }
        }

        // 4. 处理丢失 (Recovery)
        if (accessLost) {
            // 立即清理资源，下一次循环会自动进入 Step 2 的初始化逻辑
            resetCaptureState();
            QThread::msleep(500); // 避让一段时间
            continue;
        }

        // 5. 渲染与合成 (Render)
        // 即使 captureSuccess 为 false (例如 Monitor 超时)，只要 m_cachedSRV 有效，就渲染上一帧
        // 这就是 "冻结" 效果，而不是黑屏
        if (m_cachedSRV) {
            D3D11_TEXTURE2D_DESC desc;
            ComPtr<ID3D11Resource> res;
            m_cachedSRV->GetResource(&res);
            ((ID3D11Texture2D*)res.Get())->GetDesc(&desc);

            if (!m_compositionTexture || m_width != (int)desc.Width || m_height != (int)desc.Height) {
                m_stagingTexture.Reset(); m_compositionTexture.Reset(); m_compositionRTV.Reset();
                m_width = desc.Width; m_height = desc.Height;
                D3D11_TEXTURE2D_DESC compDesc = desc;
                compDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                compDesc.Usage = D3D11_USAGE_DEFAULT; compDesc.MiscFlags = 0; compDesc.CPUAccessFlags = 0;
                m_d3dDevice->CreateTexture2D(&compDesc, nullptr, &m_compositionTexture);
                m_d3dDevice->CreateRenderTargetView(m_compositionTexture.Get(), nullptr, &m_compositionRTV);
                desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
                m_d3dDevice->CreateTexture2D(&desc, nullptr, &m_stagingTexture);
                D3D11_VIEWPORT vp = {}; vp.Width = (float)m_width; vp.Height = (float)m_height; vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
                m_d3dContext->RSSetViewports(1, &vp);
            }

            float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
            m_d3dContext->ClearRenderTargetView(m_compositionRTV.Get(), clearColor);

            m_d3dContext->OMSetRenderTargets(1, m_compositionRTV.GetAddressOf(), nullptr);
            m_d3dContext->RSSetState(m_rasterizerState.Get());
            m_d3dContext->IASetInputLayout(m_inputLayout.Get());
            m_d3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            m_d3dContext->VSSetShader(m_vertexShader.Get(), nullptr, 0);
            m_d3dContext->PSSetShader(m_pixelShader.Get(), nullptr, 0);
            m_d3dContext->PSSetSamplers(0, 1, m_samplerState.GetAddressOf());

            float blendFactor[] = {0,0,0,0};
            m_d3dContext->OMSetBlendState(nullptr, blendFactor, 0xffffffff);
            renderTexture(m_cachedSRV.Get(), QRect(0, 0, m_width, m_height), true);

            processPendingUpdates();
            m_d3dContext->OMSetBlendState(m_blendState.Get(), blendFactor, 0xffffffff);
            {
                QMutexLocker locker(&m_dataMutex);
                for(auto data : m_overlays) { if (data->srv) renderTexture(data->srv, data->rect, false); }
            }

            if (m_stagingTexture) {
                m_d3dContext->CopyResource(m_stagingTexture.Get(), m_compositionTexture.Get());
                D3D11_MAPPED_SUBRESOURCE mapInfo;
                if (SUCCEEDED(m_d3dContext->Map(m_stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapInfo))) {
                    QImage img((uchar*)mapInfo.pData, m_width, m_height, mapInfo.RowPitch, QImage::Format_RGB32);
                    emit frameCaptured(img.copy());
                    if (m_snapshotRequested.fetchAndStoreAcquire(0) == 1) emit snapshotCaptured(img.copy());
                    m_d3dContext->Unmap(m_stagingTexture.Get(), 0);
                }
            }
        } else {
            QThread::msleep(10);
        }

        // 限制帧率
        QThread::msleep(16);
    }
    emit captureStopped();
}

void DxGiCapturer::renderTexture(ID3D11ShaderResourceView* srv, const QRect& rect, bool isBackground) {
    if (!srv) return;
    float left, right, top, bottom;
    if (isBackground) { left = -1.0f; right = 1.0f; top = 1.0f; bottom = -1.0f; }
    else {
        if (m_width <= 0 || m_height <= 0) return;
        left = (float)rect.x() / m_width * 2.0f - 1.0f;
        right = (float)(rect.x() + rect.width()) / m_width * 2.0f - 1.0f;
        top = 1.0f - (float)rect.y() / m_height * 2.0f;
        bottom = 1.0f - (float)(rect.y() + rect.height()) / m_height * 2.0f;
    }
    Vertex vertices[] = {
        { left, top, 0.0f, 0.0f, 0.0f }, { right, top, 0.0f, 1.0f, 0.0f }, { left, bottom, 0.0f, 0.0f, 1.0f },
        { left, bottom, 0.0f, 0.0f, 1.0f }, { right, top, 0.0f, 1.0f, 0.0f }, { right, bottom, 0.0f, 1.0f, 1.0f }
    };
    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(m_d3dContext->Map(m_vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        memcpy(map.pData, vertices, sizeof(vertices)); m_d3dContext->Unmap(m_vertexBuffer.Get(), 0);
    }
    UINT stride = sizeof(Vertex); UINT offset = 0;
    m_d3dContext->IASetVertexBuffers(0, 1, m_vertexBuffer.GetAddressOf(), &stride, &offset);
    m_d3dContext->PSSetShaderResources(0, 1, &srv);
    m_d3dContext->Draw(6, 0);
}

// ... 底部 addOverlay 等函数保持不变 ...
void DxGiCapturer::addOverlay(int id, const QImage& img, const QRect& rect) {
    QMutexLocker locker(&m_dataMutex); OverlayData* data = new OverlayData;
    data->id = id; data->image = img; data->rect = rect; data->textureDirty = true; data->rectDirty = false;
    m_overlays.insert(id, data);
}
void DxGiCapturer::updateOverlayImage(int id, const QImage& img) {
    QMutexLocker locker(&m_dataMutex); if (m_overlays.contains(id)) { OverlayData* data = m_overlays[id]; data->image = img; data->textureDirty = true; }
}
void DxGiCapturer::updateOverlayGeometry(int id, const QRect& rect) {
    QMutexLocker locker(&m_dataMutex); if (m_overlays.contains(id)) { m_overlays[id]->rect = rect; m_overlays[id]->rectDirty = true; }
}
void DxGiCapturer::removeOverlay(int id) { QMutexLocker locker(&m_dataMutex); m_pendingRemovals.append(id); }
void DxGiCapturer::requestSnapshot() { m_snapshotRequested = 1; }
void DxGiCapturer::processPendingUpdates() {
    QMutexLocker locker(&m_dataMutex);
    for(int id : m_pendingRemovals) { if(m_overlays.contains(id)) { SAFE_RELEASE(m_overlays[id]->texture); SAFE_RELEASE(m_overlays[id]->srv); delete m_overlays[id]; m_overlays.remove(id); } }
    m_pendingRemovals.clear();
    for(auto data : m_overlays) { if(data->textureDirty) updateTextureFromImage(data); }
}
void DxGiCapturer::updateTextureFromImage(OverlayData* data) {
    SAFE_RELEASE(data->texture); SAFE_RELEASE(data->srv); if (data->image.isNull()) return;
    D3D11_TEXTURE2D_DESC desc = {}; desc.Width = data->image.width(); desc.Height = data->image.height(); desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA subData = {}; subData.pSysMem = data->image.constBits(); subData.SysMemPitch = data->image.bytesPerLine();
    HRESULT hr = m_d3dDevice->CreateTexture2D(&desc, &subData, &data->texture);
    if (SUCCEEDED(hr)) m_d3dDevice->CreateShaderResourceView(data->texture, nullptr, &data->srv);
}
