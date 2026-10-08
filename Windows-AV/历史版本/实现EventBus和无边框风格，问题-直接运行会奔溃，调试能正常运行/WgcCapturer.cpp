#include "WgcCapturer.h"

// ============================================================================
// 1. 只包含 C++/WinRT 核心库
// ============================================================================
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <QDebug>

// ============================================================================
// 2. 仅包含创建 D3D 设备所需的 Interop 头文件
// ============================================================================
#include <windows.graphics.directx.direct3d11.interop.h>

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

// ============================================================================
// 3. 【核弹级修复】手动定义所有必要的 GUID 和接口
//    彻底断绝 "Dependency Hell" (依赖地狱)
// ============================================================================

// [GUID 1] IGraphicsCaptureItemInterop (用于 CreateForWindow)
// {3628E81B-3CAC-4C60-B7F4-23CE0E0C3356}
static const GUID IID_IGraphicsCaptureItemInterop_Local =
    { 0x3628E81B, 0x3CAC, 0x4C60, { 0xB7, 0xF4, 0x23, 0xCE, 0x0E, 0x0C, 0x33, 0x56 } };

// [GUID 2] IDirect3DDxgiInterfaceAccess (用于获取 D3D 纹理)
// {A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1}
static const GUID IID_IDirect3DDxgiInterfaceAccess_Local =
    { 0xA9B3D012, 0x3DF2, 0x4EE3, { 0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1 } };

// [GUID 3] IGraphicsCaptureItem (就是刚才报错缺失的那个)
// {79C3F95B-31F7-4EC2-A464-632EF5D30760}
static const GUID IID_IGraphicsCaptureItem_Local =
    { 0x79C3F95B, 0x31F7, 0x4EC2, { 0xA4, 0x64, 0x63, 0x2E, 0xF5, 0xD3, 0x07, 0x60 } };

// 接口定义：IGraphicsCaptureItemInterop
struct IGraphicsCaptureItemInterop_Local : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateForWindow(HWND window, REFIID riid, void **result) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateForMonitor(HMONITOR monitor, REFIID riid, void **result) = 0;
};

// 接口定义：IDirect3DDxgiInterfaceAccess
struct IDirect3DDxgiInterfaceAccess_Local : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID iid, void **p) = 0;
};

// ============================================================================
// 实现部分
// ============================================================================

WgcCapturer::WgcCapturer() {}

WgcCapturer::~WgcCapturer() {
    stopCapture();
}

void WgcCapturer::init(ID3D11Device* device) {
    m_d3dDevice = device;

    if (m_d3dDevice) {
        com_ptr<IDXGIDevice> dxgiDevice;
        dxgiDevice.attach(nullptr);
        m_d3dDevice->QueryInterface(__uuidof(IDXGIDevice), dxgiDevice.put_void());

        com_ptr<IInspectable> inspectable;

        // 创建 WinRT D3D 设备
        HRESULT hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put());
        if (SUCCEEDED(hr)) {
            m_winrtDevice = inspectable.as<IDirect3DDevice>();
        } else {
            qWarning() << "[WgcCapturer] Failed to create WinRT D3D device. HR:" << hr;
        }
    }
}

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

    try {
        // 1. 获取 Activation Factory
        auto activation_factory = get_activation_factory<GraphicsCaptureItem>();

        // 2. 获取 Interop 接口 (使用我们手动定义的 Local 版)
        com_ptr<IUnknown> unknownFactory = activation_factory.as<IUnknown>();
        com_ptr<IGraphicsCaptureItemInterop_Local> interopFactory;

        HRESULT hr = unknownFactory->QueryInterface(IID_IGraphicsCaptureItemInterop_Local, interopFactory.put_void());

        if (FAILED(hr) || !interopFactory) {
            qWarning() << "[WgcCapturer] Failed to get IGraphicsCaptureItemInterop. HR:" << hr;
            return false;
        }

        // 3. 通过 HWND 创建 Item
        com_ptr<IInspectable> inspectableItem;

        // 使用 IID_IGraphicsCaptureItem_Local
        hr = interopFactory->CreateForWindow(
            hwnd,
            IID_IGraphicsCaptureItem_Local,
            inspectableItem.put_void()
            );

        if (FAILED(hr)) {
            qWarning() << "[WgcCapturer] CreateForWindow Failed. HR:" << hr;
            return false;
        }

        m_item = inspectableItem.as<GraphicsCaptureItem>();
        if (!m_item) return false;

        // 4. 创建 FramePool
        m_framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            m_winrtDevice,
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            1,
            m_item.Size());

        // 5. 注册帧到达回调
        // 【修正点】第二个参数必须明确指定为 winrt::Windows::Foundation::IInspectable const&
        // 或者简单地使用 auto const&，防止与全局 COM 接口冲突
        m_frameArrivedToken = m_framePool.FrameArrived([this](Direct3D11CaptureFramePool const& sender, winrt::Windows::Foundation::IInspectable const&) {
            auto frame = sender.TryGetNextFrame();
            if (!frame) return;

            auto surface = frame.Surface();

            // 使用我们手动定义的 Access 接口
            com_ptr<IDirect3DDxgiInterfaceAccess_Local> access;
            com_ptr<IUnknown> unknownSurface = surface.as<IUnknown>();

            HRESULT hr = unknownSurface->QueryInterface(IID_IDirect3DDxgiInterfaceAccess_Local, access.put_void());

            if (SUCCEEDED(hr) && access) {
                com_ptr<ID3D11Texture2D> tex;
                // 获取真正的 D3D11 纹理
                access->GetInterface(__uuidof(ID3D11Texture2D), tex.put_void());

                if (tex) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    // 安全地将 winrt::com_ptr 转移给 WRL::ComPtr
                    m_lastFrame = tex.get();
                    m_hasNewFrame = true;

                    // 处理尺寸变化
                    auto contentSize = frame.ContentSize();
                    if (contentSize.Width != m_item.Size().Width || contentSize.Height != m_item.Size().Height) {
                        m_framePool.Recreate(
                            m_winrtDevice,
                            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                            1,
                            contentSize);
                    }
                }
            }
        });

        // 6. 启动会话
        m_session = m_framePool.CreateCaptureSession(m_item);
        m_session.IsCursorCaptureEnabled(true);
        m_session.StartCapture();

        qDebug() << "[WgcCapturer] Session Started OK.";
        return true;

    } catch (winrt::hresult_error const& ex) {
        qWarning() << "[WgcCapturer] WinRT Exception:" << QString::fromStdWString(ex.message().c_str())
                   << "HR:" << (int)ex.code();
        return false;
    } catch (...) {
        qWarning() << "[WgcCapturer] Unknown Exception.";
        return false;
    }
}

void WgcCapturer::stopCapture() {
    if (m_session) {
        m_session.Close();
        m_session = nullptr;
    }
    if (m_framePool) {
        m_framePool.FrameArrived(m_frameArrivedToken);
        m_framePool.Close();
        m_framePool = nullptr;
    }
    m_item = nullptr;
    m_lastFrame.Reset();
}

bool WgcCapturer::acquireNextFrame(ID3D11Texture2D** outTexture) {
    if (!m_hasNewFrame) return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_lastFrame) {
        m_lastFrame.CopyTo(outTexture);
        m_hasNewFrame = false;
        return true;
    }
    return false;
}
