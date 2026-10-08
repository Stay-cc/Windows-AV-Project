#include "DxGiCapturer.h"
#include <QDebug>
#include <QThread>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

#define SAFE_RELEASE(p) { if (p) { (p)->Release(); (p)=nullptr; } }

// HLSL Shader 代码
const char* shaderCode = R"(
    Texture2D shaderTexture : register(t0);
    SamplerState sampleType : register(s0);
    struct VOut { float4 position : SV_POSITION; float2 tex : TEXCOORD0; };
    VOut VShader(float4 position : POSITION, float2 tex : TEXCOORD0) {
        VOut output; output.position = position; output.tex = tex; return output;
    }
    float4 PShader(VOut input) : SV_TARGET {
        return shaderTexture.Sample(sampleType, input.tex);
    }
)";

struct Vertex { float x, y, z; float u, v; };

DxGiCapturer::DxGiCapturer(QObject *parent) : QObject(parent), m_running(0), m_snapshotRequested(0) {}
DxGiCapturer::~DxGiCapturer() { stopCapture(); cleanup(); }

QStringList DxGiCapturer::getMonitorNames() {
    QStringList list;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) return list;
    IDXGIAdapter1* adapter = nullptr; UINT adapterIndex = 0;
    while (factory->EnumAdapters1(adapterIndex++, &adapter) != DXGI_ERROR_NOT_FOUND) {
        IDXGIOutput* output = nullptr; UINT outputIndex = 0;
        while (adapter->EnumOutputs(outputIndex++, &output) != DXGI_ERROR_NOT_FOUND) {
            DXGI_OUTPUT_DESC desc; output->GetDesc(&desc);
            list.append(QString("Monitor %1: %2x%3").arg(list.size()).arg(desc.DesktopCoordinates.right - desc.DesktopCoordinates.left).arg(desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top));
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return list;
}

void DxGiCapturer::cleanup() {
    SAFE_RELEASE(m_stagingTexture);
    SAFE_RELEASE(m_compositionTexture);
    SAFE_RELEASE(m_compositionRTV);
    SAFE_RELEASE(m_deskDupl);
    SAFE_RELEASE(m_vertexShader);
    SAFE_RELEASE(m_pixelShader);
    SAFE_RELEASE(m_inputLayout);
    SAFE_RELEASE(m_vertexBuffer);
    SAFE_RELEASE(m_samplerState);
    SAFE_RELEASE(m_blendState);
    SAFE_RELEASE(m_rasterizerState);
    SAFE_RELEASE(m_depthStencilState);

    QMutexLocker locker(&m_dataMutex);
    for(auto data : m_overlays) {
        SAFE_RELEASE(data->texture);
        SAFE_RELEASE(data->srv);
        delete data;
    }
    m_overlays.clear();
    SAFE_RELEASE(m_d3dContext);
    SAFE_RELEASE(m_d3dDevice);
}

void DxGiCapturer::stopCapture() { m_running = 0; }

bool DxGiCapturer::initShaders() {
    ID3DBlob* vsBlob = nullptr; ID3DBlob* psBlob = nullptr; ID3DBlob* errorBlob = nullptr;
    HRESULT hr = D3DCompile(shaderCode, strlen(shaderCode), nullptr, nullptr, nullptr, "VShader", "vs_4_0", 0, 0, &vsBlob, &errorBlob);
    if (FAILED(hr)) { if(errorBlob) errorBlob->Release(); return false; }
    hr = m_d3dDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vertexShader);

    D3D11_INPUT_ELEMENT_DESC polygonLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    m_d3dDevice->CreateInputLayout(polygonLayout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_inputLayout);
    vsBlob->Release();

    D3DCompile(shaderCode, strlen(shaderCode), nullptr, nullptr, nullptr, "PShader", "ps_4_0", 0, 0, &psBlob, &errorBlob);
    m_d3dDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_pixelShader);
    psBlob->Release();

    D3D11_BUFFER_DESC vertexBufferDesc = {};
    vertexBufferDesc.Usage = D3D11_USAGE_DYNAMIC; vertexBufferDesc.ByteWidth = sizeof(Vertex) * 6;
    vertexBufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER; vertexBufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    m_d3dDevice->CreateBuffer(&vertexBufferDesc, nullptr, &m_vertexBuffer);

    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP; samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP; samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER; samplerDesc.MinLOD = 0; samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    m_d3dDevice->CreateSamplerState(&samplerDesc, &m_samplerState);

    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO; blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = 0x0F;
    m_d3dDevice->CreateBlendState(&blendDesc, &m_blendState);

    D3D11_RASTERIZER_DESC rasterDesc = {};
    rasterDesc.CullMode = D3D11_CULL_NONE; rasterDesc.FillMode = D3D11_FILL_SOLID; rasterDesc.DepthClipEnable = true;
    m_d3dDevice->CreateRasterizerState(&rasterDesc, &m_rasterizerState);

    D3D11_DEPTH_STENCIL_DESC depthDesc = {};
    depthDesc.DepthEnable = FALSE; depthDesc.StencilEnable = FALSE;
    m_d3dDevice->CreateDepthStencilState(&depthDesc, &m_depthStencilState);
    return true;
}

// 初始化 D3D 设备，但不初始化 Duplication（因为可能需要重试）
bool DxGiCapturer::init(int monitorIndex) {
    cleanup();
    m_currentMonitorIndex = monitorIndex; // 保存 ID 供后续使用

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) return false;

    IDXGIAdapter1* adapter = nullptr; IDXGIOutput* output = nullptr;
    IDXGIAdapter1* targetAdapter = nullptr;
    UINT adapterIdx = 0; int currentGlobalIdx = 0; bool found = false;

    // 1. 查找对应的适配器
    while (!found && factory->EnumAdapters1(adapterIdx++, &adapter) != DXGI_ERROR_NOT_FOUND) {
        UINT outputIdx = 0;
        while (adapter->EnumOutputs(outputIdx++, &output) != DXGI_ERROR_NOT_FOUND) {
            if (currentGlobalIdx == monitorIndex) {
                targetAdapter = adapter; found = true;
                output->Release(); // 只要 Adapter，不需要 Output 接口
                break;
            }
            output->Release(); currentGlobalIdx++;
        }
        if (!found) adapter->Release();
    }
    factory->Release();

    if (!found) return false;

    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL featureLevel;

    // 2. 创建设备
    HRESULT hr = D3D11CreateDevice(targetAdapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, featureLevels, 1, D3D11_SDK_VERSION, &m_d3dDevice, &featureLevel, &m_d3dContext);

    SAFE_RELEASE(targetAdapter);

    if (SUCCEEDED(hr)) {
        if (!initShaders()) return false;
        // 注意：这里不直接调用 initDuplication，让 startCapture 循环去处理
        // 这样如果启动时就是任务管理器界面，也能自动重试
        return true;
    }
    return false;
}

// 【新增】尝试建立 Desktop Duplication 连接
bool DxGiCapturer::initDuplication() {
    SAFE_RELEASE(m_deskDupl);

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) return false;

    IDXGIAdapter1* adapter = nullptr; IDXGIOutput* output = nullptr;
    UINT adapterIdx = 0; int currentGlobalIdx = 0; bool found = false;

    // 重新根据 ID 查找 Output
    while (!found && factory->EnumAdapters1(adapterIdx++, &adapter) != DXGI_ERROR_NOT_FOUND) {
        UINT outputIdx = 0;
        while (adapter->EnumOutputs(outputIdx++, &output) != DXGI_ERROR_NOT_FOUND) {
            if (currentGlobalIdx == m_currentMonitorIndex) {
                found = true; break;
            }
            output->Release(); currentGlobalIdx++;
        }
        if (!found) adapter->Release();
    }
    factory->Release();

    if (!found) return false;

    IDXGIOutput1* output1 = nullptr;
    HRESULT hr = E_FAIL;
    if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1))) {
        // 创建 Duplication 接口
        hr = output1->DuplicateOutput(m_d3dDevice, &m_deskDupl);
        output1->Release();
    }

    SAFE_RELEASE(output);
    SAFE_RELEASE(adapter);

    return SUCCEEDED(hr);
}

// ... Overlay 相关函数保持不变 ...
void DxGiCapturer::addOverlay(int id, const QImage& img, const QRect& rect) {
    QMutexLocker locker(&m_dataMutex);
    OverlayData* data = new OverlayData;
    data->id = id; data->image = img; data->rect = rect;
    data->textureDirty = true; data->rectDirty = false;
    m_overlays.insert(id, data);
}

void DxGiCapturer::updateOverlayImage(int id, const QImage& img) {
    QMutexLocker locker(&m_dataMutex);
    if (m_overlays.contains(id)) {
        OverlayData* data = m_overlays[id];
        data->image = img; data->textureDirty = true;
    }
}

void DxGiCapturer::updateOverlayGeometry(int id, const QRect& rect) {
    QMutexLocker locker(&m_dataMutex);
    if (m_overlays.contains(id)) {
        m_overlays[id]->rect = rect; m_overlays[id]->rectDirty = true;
    }
}

void DxGiCapturer::removeOverlay(int id) {
    QMutexLocker locker(&m_dataMutex);
    m_pendingRemovals.append(id);
}

void DxGiCapturer::requestSnapshot() { m_snapshotRequested = 1; }

void DxGiCapturer::processPendingUpdates() {
    QMutexLocker locker(&m_dataMutex);
    for(int id : m_pendingRemovals) {
        if(m_overlays.contains(id)) {
            SAFE_RELEASE(m_overlays[id]->texture); SAFE_RELEASE(m_overlays[id]->srv);
            delete m_overlays[id]; m_overlays.remove(id);
        }
    }
    m_pendingRemovals.clear();
    for(auto data : m_overlays) {
        if(data->textureDirty) updateTextureFromImage(data);
    }
}

void DxGiCapturer::updateTextureFromImage(OverlayData* data) {
    SAFE_RELEASE(data->texture); SAFE_RELEASE(data->srv);
    if (data->image.isNull()) return;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = data->image.width(); desc.Height = data->image.height();
    desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA subData = {};
    subData.pSysMem = data->image.constBits(); subData.SysMemPitch = data->image.bytesPerLine();
    HRESULT hr = m_d3dDevice->CreateTexture2D(&desc, &subData, &data->texture);
    if (SUCCEEDED(hr)) m_d3dDevice->CreateShaderResourceView(data->texture, nullptr, &data->srv);
}

void DxGiCapturer::renderOverlay(ID3D11ShaderResourceView* srv, const QRect& rect) {
    // ... 保持原有实现 ...
    if (!srv || m_width <= 0 || m_height <= 0) return;
    float left = (float)rect.x() / m_width * 2.0f - 1.0f;
    float right = (float)(rect.x() + rect.width()) / m_width * 2.0f - 1.0f;
    float top = 1.0f - (float)rect.y() / m_height * 2.0f;
    float bottom = 1.0f - (float)(rect.y() + rect.height()) / m_height * 2.0f;
    Vertex vertices[] = {
        { left, top, 0.0f, 0.0f, 0.0f }, { right, top, 0.0f, 1.0f, 0.0f }, { left, bottom, 0.0f, 0.0f, 1.0f },
        { left, bottom, 0.0f, 0.0f, 1.0f }, { right, top, 0.0f, 1.0f, 0.0f }, { right, bottom, 0.0f, 1.0f, 1.0f }
    };
    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(m_d3dContext->Map(m_vertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        memcpy(map.pData, vertices, sizeof(vertices)); m_d3dContext->Unmap(m_vertexBuffer, 0);
    }
    UINT stride = sizeof(Vertex); UINT offset = 0;
    m_d3dContext->IASetVertexBuffers(0, 1, &m_vertexBuffer, &stride, &offset);
    m_d3dContext->PSSetShaderResources(0, 1, &srv);
    m_d3dContext->Draw(6, 0);
}

// ----------------------------------------------------------------------
// 【核心修改】企业级采集循环：支持掉线自动重连
// ----------------------------------------------------------------------
void DxGiCapturer::startCapture() {
    if (m_running) return;
    m_running = 1;

    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    IDXGIResource* desktopResource = nullptr;
    ID3D11Texture2D* acquiredDesktopImage = nullptr;

    while (m_running) {
        // 1. 如果 Duplication 接口无效（比如刚启动，或之前 Access Lost 导致释放了），尝试初始化
        if (!m_deskDupl) {
            if (initDuplication()) {
                qDebug() << "DxGiCapturer: Duplication interface initialized.";
            } else {
                // 初始化失败（可能还在安全桌面上），等待并重试
                QThread::msleep(500);
                continue;
            }
        }

        // 2. 尝试获取桌面帧
        HRESULT hr = m_deskDupl->AcquireNextFrame(10, &frameInfo, &desktopResource);

        bool hasDesktopFrame = false;

        if (SUCCEEDED(hr)) {
            // 获取成功
            hr = desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&acquiredDesktopImage);
            SAFE_RELEASE(desktopResource);
            if (SUCCEEDED(hr)) hasDesktopFrame = true;
            else m_deskDupl->ReleaseFrame();
        } else if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            // 超时（桌面静止），这不是错误，可以复用上一帧或仅渲染 Overlay
            hasDesktopFrame = false;
        } else if (hr == DXGI_ERROR_ACCESS_LOST) {
            // 【关键修复】检测到访问丢失（如切到任务管理器），释放接口，下一次循环会自动尝试 initDuplication
            qWarning() << "DxGiCapturer: Access Lost. Resetting duplication interface...";
            SAFE_RELEASE(m_deskDupl);
            SAFE_RELEASE(acquiredDesktopImage); // 确保释放
            // 不退出 m_running，只是 continue
            continue;
        } else if (FAILED(hr)) {
            // 其他严重错误
            qWarning() << "DxGiCapturer: Acquire failed with code:" << hr;
            SAFE_RELEASE(m_deskDupl); // 尝试重建作为防御
            continue;
        }

        // 3. 准备渲染目标
        if (hasDesktopFrame) {
            D3D11_TEXTURE2D_DESC desc; acquiredDesktopImage->GetDesc(&desc);
            if (!m_compositionTexture || m_width != (int)desc.Width || m_height != (int)desc.Height) {
                // 尺寸变化，重建纹理
                SAFE_RELEASE(m_stagingTexture); SAFE_RELEASE(m_compositionTexture); SAFE_RELEASE(m_compositionRTV);
                m_width = desc.Width; m_height = desc.Height;

                D3D11_TEXTURE2D_DESC compDesc = desc;
                compDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                compDesc.Usage = D3D11_USAGE_DEFAULT; compDesc.MiscFlags = 0; compDesc.CPUAccessFlags = 0;
                m_d3dDevice->CreateTexture2D(&compDesc, nullptr, &m_compositionTexture);
                m_d3dDevice->CreateRenderTargetView(m_compositionTexture, nullptr, &m_compositionRTV);

                desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
                m_d3dDevice->CreateTexture2D(&desc, nullptr, &m_stagingTexture);
            }
            m_d3dContext->CopyResource(m_compositionTexture, acquiredDesktopImage);
            SAFE_RELEASE(acquiredDesktopImage);
            m_deskDupl->ReleaseFrame();
        } else {
            // 没新帧但接口正常，且有旧纹理，可以继续合成
            if (!m_compositionTexture) { QThread::msleep(10); continue; }
        }

        // 4. 更新摄像头等 Overlay 数据
        processPendingUpdates();

        // 5. 合成渲染
        m_d3dContext->OMSetRenderTargets(1, &m_compositionRTV, nullptr);

        D3D11_VIEWPORT vp = {}; vp.Width = (float)m_width; vp.Height = (float)m_height; vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
        m_d3dContext->RSSetViewports(1, &vp);

        m_d3dContext->RSSetState(m_rasterizerState);
        m_d3dContext->OMSetDepthStencilState(m_depthStencilState, 0);
        m_d3dContext->IASetInputLayout(m_inputLayout);
        m_d3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_d3dContext->VSSetShader(m_vertexShader, nullptr, 0);
        m_d3dContext->PSSetShader(m_pixelShader, nullptr, 0);
        m_d3dContext->PSSetSamplers(0, 1, &m_samplerState);
        float blendFactor[] = {0,0,0,0};
        m_d3dContext->OMSetBlendState(m_blendState, blendFactor, 0xffffffff);

        {
            QMutexLocker locker(&m_dataMutex);
            for(auto data : m_overlays) {
                if (data->srv) renderOverlay(data->srv, data->rect);
            }
        }

        // 6. 回传数据给 UI
        if (m_stagingTexture) {
            m_d3dContext->CopyResource(m_stagingTexture, m_compositionTexture);
            D3D11_MAPPED_SUBRESOURCE mapInfo;
            if (SUCCEEDED(m_d3dContext->Map(m_stagingTexture, 0, D3D11_MAP_READ, 0, &mapInfo))) {
                QImage img((uchar*)mapInfo.pData, m_width, m_height, mapInfo.RowPitch, QImage::Format_ARGB32_Premultiplied);
                emit frameCaptured(img.copy());
                if (m_snapshotRequested.fetchAndStoreAcquire(0) == 1) emit snapshotCaptured(img.copy());
                m_d3dContext->Unmap(m_stagingTexture, 0);
            }
        }

        QThread::msleep(8);
    }
    emit captureStopped();
}
