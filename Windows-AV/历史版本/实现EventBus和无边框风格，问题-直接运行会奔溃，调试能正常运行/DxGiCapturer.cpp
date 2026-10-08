#include "DxGiCapturer.h"
#include <QDebug>
#include <QThread>
#include <QCoreApplication>
#include <dwmapi.h>
#include <QElapsedTimer>
#include <QDateTime>
#include <cmath>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowsapp.lib")

#define SAFE_RELEASE(p) { if(p) { (p)->Release(); (p)=nullptr; } }

const double TARGET_FPS = 30.0;
const double TARGET_FRAME_TIME_SEC = 1.0 / TARGET_FPS;

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
            TCHAR title[256]; GetWindowText(hwnd, title, 256);
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

DxGiCapturer::DxGiCapturer(QObject *parent) : QObject(parent), m_running(0), m_snapshotRequested(0) {
    m_previewEnabled = true;
    m_isRecordingMP4 = false;
    m_uiInFlight = 0;
    m_recorderInFlight = 0;
    m_fpsCounter = 0;
    m_width = 0;
    m_height = 0;
    m_wgcCapturer = nullptr;
    m_targetChanged = false;
}

DxGiCapturer::~DxGiCapturer() {
    stopCapture();
    if(m_running) { m_running = 0; QThread::msleep(50); }
    cleanup();
}

void DxGiCapturer::setPreviewEnabled(bool enabled) {
    m_previewEnabled = enabled;
    if (!enabled) m_uiInFlight = 0;
}

static long long s_recordedFrameCount = 0;

void DxGiCapturer::startRecording() {
    m_recorderInFlight = 0;
    m_isRecordingMP4 = true;
    s_recordedFrameCount = 0;
    m_timestampTimer.start();
    qDebug() << "[DxGiCapturer] MP4 Recording Started.";
}

void DxGiCapturer::stopRecording() {
    m_isRecordingMP4 = false;
    qDebug() << "[DxGiCapturer] MP4 Recording Stopped. Total Frames:" << s_recordedFrameCount;
}

void DxGiCapturer::onUiFrameProcessed() { m_uiInFlight = 0; }
void DxGiCapturer::onRecorderFrameProcessed() { m_recorderInFlight--; if (m_recorderInFlight < 0) m_recorderInFlight = 0; }

QStringList DxGiCapturer::getMonitorNames() {
    QStringList list;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) return list;

    ComPtr<IDXGIAdapter1> adapter; UINT adapterIndex = 0;
    int globalMonitorIndex = 0;

    while (factory->EnumAdapters1(adapterIndex++, &adapter) != DXGI_ERROR_NOT_FOUND) {
        ComPtr<IDXGIOutput> output; UINT outputIndex = 0;
        while (adapter->EnumOutputs(outputIndex++, &output) != DXGI_ERROR_NOT_FOUND) {
            DXGI_OUTPUT_DESC desc; output->GetDesc(&desc);
            int w = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
            int h = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
            list.append(QString("Monitor %1: %2x%3").arg(globalMonitorIndex++).arg(w).arg(h));
        }
    }
    return list;
}

void DxGiCapturer::cleanup() {
    qDebug() << "[DxGiCapturer] Cleanup started.";
    resetCaptureState(); // 确保这里调用了完整的重置

    // 释放 Shader 和管线状态
    m_vertexShader.Reset();
    m_pixelShader.Reset();
    m_inputLayout.Reset();
    m_vertexBuffer.Reset();
    m_samplerState.Reset();
    m_blendState.Reset();
    m_rasterizerState.Reset();
    m_depthStencilState.Reset();

    QMutexLocker locker(&m_dataMutex);
    for(auto data : m_overlays) {
        SAFE_RELEASE(data->texture);
        SAFE_RELEASE(data->srv);
        delete data;
    }
    m_overlays.clear();

    if (m_d3dContext) { m_d3dContext->ClearState(); m_d3dContext->Flush(); }
    m_d3dContext.Reset();
    m_d3dDevice.Reset();
    qDebug() << "[DxGiCapturer] Cleanup finished.";
}

void DxGiCapturer::resetCaptureState() {
    if (m_wgcCapturer) {
        delete m_wgcCapturer;
        m_wgcCapturer = nullptr;
    }

    // 释放桌面复制接口
    m_deskDupl.Reset();

    // 释放所有纹理资源
    // 之前因为没有释放 m_compositionTexture，导致切换 GPU (显示器) 时
    // 新的 Context 试图使用旧 Device 创建的纹理，导致渲染失败。
    m_currentMonitorTexture.Reset();
    m_cachedSRV.Reset();
    m_capturedTexture.Reset();

    m_compositionTexture.Reset(); // [新增] 必须释放
    m_compositionRTV.Reset();     // [新增] 必须释放
    m_stagingTexture.Reset();

    // 注意：不要在这里释放 m_overlays 里的纹理，
    // 它们会在 processPendingUpdates 里的 updateTextureFromImage 根据新 Device 自动重建
}

void DxGiCapturer::stopCapture() { m_running = 0; }

bool DxGiCapturer::initShaders() {
    if (!m_d3dDevice) return false;
    ID3DBlob* vsBlob = nullptr; ID3DBlob* psBlob = nullptr;
    HRESULT hr = D3DCompile(shaderCode, strlen(shaderCode), nullptr, nullptr, nullptr, "VShader", "vs_4_0", 0, 0, &vsBlob, nullptr);
    if(FAILED(hr)) return false;
    m_d3dDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vertexShader);
    D3D11_INPUT_ELEMENT_DESC layout[] = { { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 }, { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
    m_d3dDevice->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_inputLayout);
    vsBlob->Release();
    hr = D3DCompile(shaderCode, strlen(shaderCode), nullptr, nullptr, nullptr, "PShader", "ps_4_0", 0, 0, &psBlob, nullptr);
    if(FAILED(hr)) return false;
    m_d3dDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_pixelShader);
    psBlob->Release();
    D3D11_BUFFER_DESC vbDesc = { sizeof(Vertex) * 6, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
    m_d3dDevice->CreateBuffer(&vbDesc, nullptr, &m_vertexBuffer);
    D3D11_SAMPLER_DESC sampDesc = {}; sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sampDesc.AddressU = sampDesc.AddressV = sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    m_d3dDevice->CreateSamplerState(&sampDesc, &m_samplerState);
    D3D11_BLEND_DESC blendDesc = {}; blendDesc.RenderTarget[0].BlendEnable = TRUE; blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD; blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO; blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD; blendDesc.RenderTarget[0].RenderTargetWriteMask = 0x0F;
    m_d3dDevice->CreateBlendState(&blendDesc, &m_blendState);
    D3D11_RASTERIZER_DESC rsDesc = {}; rsDesc.CullMode = D3D11_CULL_NONE; rsDesc.FillMode = D3D11_FILL_SOLID;
    m_d3dDevice->CreateRasterizerState(&rsDesc, &m_rasterizerState);
    D3D11_DEPTH_STENCIL_DESC dsDesc = {}; m_d3dDevice->CreateDepthStencilState(&dsDesc, &m_depthStencilState);
    return true;
}

bool DxGiCapturer::initDevice(IDXGIAdapter* specificAdapter) {
    cleanup();
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    UINT numFeatureLevels = ARRAYSIZE(featureLevels);
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = E_FAIL;
    UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT; // 必须支持 BGRA，Qt 需要

    qDebug() << "[DxGiCapturer] Creating D3D11 Device. Adapter:" << (specificAdapter ? "Specific" : "Default");

    hr = D3D11CreateDevice(specificAdapter, specificAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevels, numFeatureLevels, D3D11_SDK_VERSION, m_d3dDevice.ReleaseAndGetAddressOf(), &featureLevel, m_d3dContext.ReleaseAndGetAddressOf());

    if (FAILED(hr) && !specificAdapter) {
        qWarning() << "[DxGiCapturer] Hardware Init Failed. Trying WARP...";
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevels, numFeatureLevels, D3D11_SDK_VERSION, m_d3dDevice.ReleaseAndGetAddressOf(), &featureLevel, m_d3dContext.ReleaseAndGetAddressOf());
    }

    if (SUCCEEDED(hr)) {
        qDebug() << "[DxGiCapturer] Device Created! Feature Level:" << featureLevel;
        return initShaders();
    }

    qCritical() << "[DxGiCapturer] FATAL: D3D init failed. HR:" << hr;
    return false;
}

bool DxGiCapturer::init() { return initDevice(nullptr); }

bool DxGiCapturer::initDuplication(int monitorIndex, ComPtr<IDXGIOutputDuplication>& outDupl) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) return false;

    ComPtr<IDXGIAdapter1> adapter; UINT adapterIndex = 0;
    int currentGlobalIndex = 0;

    while (factory->EnumAdapters1(adapterIndex++, &adapter) != DXGI_ERROR_NOT_FOUND) {
        ComPtr<IDXGIOutput> output; UINT outputIndex = 0;
        while (adapter->EnumOutputs(outputIndex++, &output) != DXGI_ERROR_NOT_FOUND) {

            if (currentGlobalIndex == monitorIndex) {
                DXGI_ADAPTER_DESC1 targetDesc; adapter->GetDesc1(&targetDesc);

                ComPtr<IDXGIDevice> currentDxgiDev;
                ComPtr<IDXGIAdapter> currentAdapter;
                DXGI_ADAPTER_DESC currentDesc = {};

                bool needRecreate = true;
                if (m_d3dDevice && SUCCEEDED(m_d3dDevice.As(&currentDxgiDev)) &&
                    SUCCEEDED(currentDxgiDev->GetAdapter(&currentAdapter))) {
                    currentAdapter->GetDesc(&currentDesc);
                    if (currentDesc.AdapterLuid.LowPart == targetDesc.AdapterLuid.LowPart &&
                        currentDesc.AdapterLuid.HighPart == targetDesc.AdapterLuid.HighPart) {
                        needRecreate = false;
                    }
                }

                if (needRecreate) {
                    qDebug() << "[DxGiCapturer] Monitor is on a different GPU. Recreating Device...";
                    ComPtr<IDXGIAdapter> targetAdapter;
                    adapter.As(&targetAdapter);
                    if (!initDevice(targetAdapter.Get())) return false;
                }

                ComPtr<IDXGIOutput1> output1;
                if (SUCCEEDED(output.As(&output1))) {
                    HRESULT hr = output1->DuplicateOutput(m_d3dDevice.Get(), &outDupl);
                    if (SUCCEEDED(hr)) return true;
                    if (hr == E_ACCESSDENIED) qWarning() << "[DxGiCapturer] Access Denied.";
                }
                return false;
            }
            currentGlobalIndex++;
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

void DxGiCapturer::preciseSleep(double targetSeconds) {
    if (targetSeconds <= 0) return;
    double targetMs = targetSeconds * 1000.0;
    QElapsedTimer t; t.start();
    if (targetMs > 2.0) QThread::msleep((unsigned long)(targetMs - 1.0));
    while (t.nsecsElapsed() / 1000000000.0 < targetSeconds) { }
}

void DxGiCapturer::startCapture() {
    if (m_running) return;
    m_running = 1;
    m_uiInFlight = 0;
    m_recorderInFlight = 0;

    { QMutexLocker locker(&m_targetMutex); m_currentTarget = m_pendingTarget; m_targetChanged = true; }

    if (!m_d3dDevice) initDevice(nullptr);

    qDebug() << "[DxGiCapturer] Loop Started. Thread ID:" << (quintptr)QThread::currentThreadId();

    DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
    ComPtr<IDXGIResource> desktopResource;

    qint64 lastUiEmitTime = 0;
    m_fpsCounter = 0;
    m_logTimer.start();
    m_loopTimer.start();

    double nextFrameTargetTime = 0.0;

    while (m_running) {
        QCoreApplication::processEvents();

        if (m_logTimer.elapsed() >= 1000) {
            int fps = m_fpsCounter.load();
            if (fps > 0) qDebug() << "[STATS] Capture FPS:" << fps;
            m_fpsCounter = 0;
            m_logTimer.restart();
        }

        bool needSwitch = false;
        {
            QMutexLocker locker(&m_targetMutex);
            if (m_targetChanged) {
                m_currentTarget = m_pendingTarget;
                m_targetChanged = false;
                needSwitch = true;
            }
        }
        if (needSwitch) resetCaptureState();

        bool isMonitor = (m_currentTarget.type == CaptureSourceType::Monitor);
        bool isWindow = (m_currentTarget.type == CaptureSourceType::Window);
        bool initialized = false;

        if (isMonitor) {
            if (!m_deskDupl) {
                if (initDuplication(m_currentTarget.monitorIndex, m_deskDupl)) {
                    qDebug() << "[DxGiCapturer] Monitor Init Success.";
                    initialized = true;
                } else { QThread::msleep(200); continue; }
            } else initialized = true;
        } else if (isWindow) {
            if (!m_wgcCapturer) {
                if (!IsWindow(m_currentTarget.windowHandle) || IsIconic(m_currentTarget.windowHandle)) {
                    QThread::msleep(200); continue;
                }
                HWND rootHwnd = GetRootWindow(m_currentTarget.windowHandle);
                m_wgcCapturer = new WgcCapturer();
                if (!m_d3dDevice) initDevice(nullptr);
                if (m_d3dDevice) {
                    m_wgcCapturer->init(m_d3dDevice.Get());
                    if (m_wgcCapturer->startCapture(rootHwnd)) {
                        qDebug() << "[DxGiCapturer] Window Init Success.";
                        initialized = true;
                    } else { delete m_wgcCapturer; m_wgcCapturer = nullptr; QThread::msleep(200); continue; }
                }
            } else initialized = true;
        }

        if (!initialized) { QThread::msleep(50); continue; }

        bool captureSuccess = false;
        bool accessLost = false;

        if (isMonitor && m_deskDupl) {
            desktopResource.Reset();
            ZeroMemory(&frameInfo, sizeof(frameInfo));
            HRESULT hr = m_deskDupl->AcquireNextFrame(100, &frameInfo, &desktopResource);

            if (SUCCEEDED(hr)) {
                ComPtr<ID3D11Texture2D> rawTex;
                if (desktopResource) hr = desktopResource.As(&rawTex);

                if (SUCCEEDED(hr) && rawTex) {
                    D3D11_TEXTURE2D_DESC desc; rawTex->GetDesc(&desc);
                    if (desc.Width > 0 && desc.Height > 0) {
                        if (!m_currentMonitorTexture || desc.Width != m_width || desc.Height != m_height) {
                            qDebug() << "[DxGiCapturer] Recreating Cache Texture: " << desc.Width << "x" << desc.Height;
                            m_currentMonitorTexture.Reset();
                            m_cachedSRV.Reset();
                            m_stagingTexture.Reset();
                            m_width = desc.Width;
                            m_height = desc.Height;

                            D3D11_TEXTURE2D_DESC cacheDesc = desc;
                            cacheDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                            cacheDesc.MiscFlags = 0;
                            cacheDesc.CPUAccessFlags = 0;
                            cacheDesc.Usage = D3D11_USAGE_DEFAULT;

                            hr = m_d3dDevice->CreateTexture2D(&cacheDesc, nullptr, &m_currentMonitorTexture);
                            if(FAILED(hr)) { m_deskDupl->ReleaseFrame(); continue; }
                            hr = m_d3dDevice->CreateShaderResourceView(m_currentMonitorTexture.Get(), nullptr, &m_cachedSRV);
                            if(FAILED(hr)) { m_deskDupl->ReleaseFrame(); continue; }
                        }
                        if(m_currentMonitorTexture && m_d3dContext) {
                            m_d3dContext->CopyResource(m_currentMonitorTexture.Get(), rawTex.Get());
                            captureSuccess = true;
                        }
                    }
                }
                m_deskDupl->ReleaseFrame();
            } else if (hr == DXGI_ERROR_ACCESS_LOST) {
                accessLost = true;
            }
            else if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
                // [新增] 如果超时（画面静止），我们应该继续使用上一帧 (m_cachedSRV)
                // 只要 m_cachedSRV 不为空，就可以继续渲染，避免黑屏
            }
        } else if (isWindow && m_wgcCapturer) {
            ID3D11Texture2D* wgcTex = nullptr;
            if (m_wgcCapturer->acquireNextFrame(&wgcTex)) {
                m_capturedTexture.Attach(wgcTex); m_cachedSRV.Reset();
                m_d3dDevice->CreateShaderResourceView(m_capturedTexture.Get(), nullptr, &m_cachedSRV);
                captureSuccess = true;
            }
        }

        if (accessLost) { resetCaptureState(); QThread::msleep(100); continue; }

        if (m_cachedSRV) {
            qint64 fixedTimestamp = (qint64)(s_recordedFrameCount * 1000.0 / TARGET_FPS);

            // 【核心修复开始】 -------------------------------------------------------
            // 旧代码: if (!m_compositionTexture || m_width <= 1 || m_height <= 1)
            // 问题: 分辨率变化时 m_stagingTexture 被 Reset 了，但这里没检查，导致跳过重建

            bool needRecreate = false;
            if (!m_compositionTexture || !m_stagingTexture || m_width <= 1 || m_height <= 1) {
                needRecreate = true;
            } else {
                // 进一步检查尺寸是否匹配
                D3D11_TEXTURE2D_DESC curDesc;
                m_compositionTexture->GetDesc(&curDesc);
                if (curDesc.Width != (UINT)m_width || curDesc.Height != (UINT)m_height) {
                    needRecreate = true;
                }
            }

            if (needRecreate) {
                // 如果需要重建，先清理旧的
                m_compositionTexture.Reset();
                m_compositionRTV.Reset();
                m_stagingTexture.Reset();

                D3D11_TEXTURE2D_DESC desc;
                ComPtr<ID3D11Resource> res;
                m_cachedSRV->GetResource(&res);
                if(res) ((ID3D11Texture2D*)res.Get())->GetDesc(&desc);
                else continue;

                // 强制使用当前检测到的宽高
                m_width = desc.Width; m_height = desc.Height;
                if (m_width <= 1 || m_height <= 1) continue;

                // 创建合成纹理 (BGRA)
                D3D11_TEXTURE2D_DESC compDesc = desc;
                compDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                compDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                compDesc.Usage = D3D11_USAGE_DEFAULT;
                compDesc.MiscFlags = 0;
                compDesc.CPUAccessFlags = 0;

                HRESULT hrComp = m_d3dDevice->CreateTexture2D(&compDesc, nullptr, &m_compositionTexture);
                if (FAILED(hrComp)) { qWarning() << "Create Composition Failed"; continue; }

                m_d3dDevice->CreateRenderTargetView(m_compositionTexture.Get(), nullptr, &m_compositionRTV);

                // 创建 Staging 纹理 (用于 CPU 读取)
                D3D11_TEXTURE2D_DESC stageDesc = compDesc;
                stageDesc.Usage = D3D11_USAGE_STAGING;
                stageDesc.BindFlags = 0;
                stageDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

                HRESULT hrStage = m_d3dDevice->CreateTexture2D(&stageDesc, nullptr, &m_stagingTexture);
                if (FAILED(hrStage)) { qWarning() << "Create Staging Failed"; continue; }

                D3D11_VIEWPORT vp = {};
                vp.Width = (float)m_width; vp.Height = (float)m_height;
                vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
                m_d3dContext->RSSetViewports(1, &vp);
            }

            // 渲染过程（Shader 会自动处理颜色空间转换，如 HDR -> SDR）
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
            { QMutexLocker locker(&m_dataMutex); for(auto data : m_overlays) if (data->srv) renderTexture(data->srv, data->rect, false); }

            if (m_stagingTexture) {
                m_d3dContext->CopyResource(m_stagingTexture.Get(), m_compositionTexture.Get());

                D3D11_MAPPED_SUBRESOURCE mapInfo = {};
                // 【核心修复】强制 Flush，确保拷贝完成
                m_d3dContext->Flush();

                HRESULT hrMap = m_d3dContext->Map(m_stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapInfo);
                if (SUCCEEDED(hrMap) && mapInfo.pData) {
                    try {
                        QImage deepCopy;
                        int expectedBytesPerLine = m_width * 4; // B8G8R8A8 = 4 bytes

                        // 显卡返回的 Pitch 可能会有填充，必须处理
                        if (mapInfo.RowPitch == (UINT)expectedBytesPerLine) {
                            QImage wrapped((uchar*)mapInfo.pData, m_width, m_height, QImage::Format_ARGB32_Premultiplied);
                            deepCopy = wrapped.copy();
                        } else {
                            // 手动逐行拷贝，防止读取 Padding 区域越界
                            deepCopy = QImage(m_width, m_height, QImage::Format_ARGB32_Premultiplied);
                            uchar* dst = deepCopy.bits();
                            uchar* src = (uchar*)mapInfo.pData;
                            for (int y = 0; y < m_height; ++y) {
                                memcpy(dst + y * expectedBytesPerLine, src + y * mapInfo.RowPitch, expectedBytesPerLine);
                            }
                        }

                        m_d3dContext->Unmap(m_stagingTexture.Get(), 0);

                        if (m_isRecordingMP4 && m_running) {
                            emit frameCaptured(deepCopy, fixedTimestamp);
                            s_recordedFrameCount++;
                            m_fpsCounter++;
                        }
                        if (m_previewEnabled) {
                            qint64 now = QDateTime::currentMSecsSinceEpoch();
                            if (now - lastUiEmitTime >= 33 && m_uiInFlight == 0) {
                                m_uiInFlight = 1;
                                lastUiEmitTime = now;
                                emit frameCapturedForPreview(deepCopy);
                            }
                        }
                        if (m_snapshotRequested.fetchAndStoreAcquire(0) == 1) emit snapshotCaptured(deepCopy);
                    } catch (...) {
                        m_d3dContext->Unmap(m_stagingTexture.Get(), 0);
                        qWarning() << "[DxGiCapturer] Exception during copy.";
                    }
                } else {
                    if (SUCCEEDED(hrMap)) m_d3dContext->Unmap(m_stagingTexture.Get(), 0);
                }
            }
        }

        nextFrameTargetTime += TARGET_FRAME_TIME_SEC;
        double currentTime = m_loopTimer.nsecsElapsed() / 1000000000.0;
        double sleepTime = nextFrameTargetTime - currentTime;
        if (sleepTime < -1.0) nextFrameTargetTime = currentTime;
        else if (sleepTime > 0) preciseSleep(sleepTime);
    }
    qDebug() << "[DxGiCapturer] Loop Stopped.";
    emit captureStopped();
}

void DxGiCapturer::renderTexture(ID3D11ShaderResourceView* srv, const QRect& rect, bool isBackground) {
    if (!srv || !m_d3dDevice || !m_vertexBuffer) return;

    float left, right, top, bottom;
    if (isBackground) { left = -1.0f; right = 1.0f; top = 1.0f; bottom = -1.0f; }
    else {
        float w = (float)m_width; float h = (float)m_height;
        if (w <= 1 || h <= 1) return;
        left = (float)rect.x() / w * 2.0f - 1.0f;
        right = (float)(rect.x() + rect.width()) / w * 2.0f - 1.0f;
        top = 1.0f - (float)rect.y() / h * 2.0f;
        bottom = 1.0f - (float)(rect.y() + rect.height()) / h * 2.0f;
    }
    Vertex vertices[] = { { left, top, 0.0f, 0.0f, 0.0f }, { right, top, 0.0f, 1.0f, 0.0f }, { left, bottom, 0.0f, 0.0f, 1.0f }, { left, bottom, 0.0f, 0.0f, 1.0f }, { right, top, 0.0f, 1.0f, 0.0f }, { right, bottom, 0.0f, 1.0f, 1.0f } };

    D3D11_MAPPED_SUBRESOURCE map = {};
    HRESULT hr = m_d3dContext->Map(m_vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
    if (SUCCEEDED(hr)) {
        if (map.pData) {
            memcpy(map.pData, vertices, sizeof(vertices));
            m_d3dContext->Unmap(m_vertexBuffer.Get(), 0);
        } else {
            m_d3dContext->Unmap(m_vertexBuffer.Get(), 0);
            return;
        }
    } else {
        return;
    }

    UINT stride = sizeof(Vertex); UINT offset = 0;
    m_d3dContext->IASetVertexBuffers(0, 1, m_vertexBuffer.GetAddressOf(), &stride, &offset);
    m_d3dContext->PSSetShaderResources(0, 1, &srv);
    m_d3dContext->Draw(6, 0);
}

void DxGiCapturer::addOverlay(int id, const QImage& img, const QRect& rect) {
    if (!m_d3dDevice) return;
    QMutexLocker locker(&m_dataMutex);
    OverlayData* data = new OverlayData; data->id = id; data->image = img; data->rect = rect; data->textureDirty = true; data->rectDirty = false; m_overlays.insert(id, data);
}
void DxGiCapturer::updateOverlayImage(int id, const QImage& img) {
    if (!m_d3dDevice) return;
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
    if (!m_d3dDevice) return;
    SAFE_RELEASE(data->texture); SAFE_RELEASE(data->srv); if (data->image.isNull()) return;

    QImage converted = data->image;
    if (converted.format() != QImage::Format_RGBA8888) converted = converted.convertToFormat(QImage::Format_RGBA8888);

    D3D11_TEXTURE2D_DESC desc = {}; desc.Width = converted.width(); desc.Height = converted.height(); desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA subData = {}; subData.pSysMem = converted.constBits(); subData.SysMemPitch = converted.bytesPerLine();

    HRESULT hr = m_d3dDevice->CreateTexture2D(&desc, &subData, &data->texture);
    if (SUCCEEDED(hr)) m_d3dDevice->CreateShaderResourceView(data->texture, nullptr, &data->srv);
    data->textureDirty = false;
}
