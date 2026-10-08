#include "WinRTCamera.h"
#include <windows.graphics.directx.direct3d11.interop.h>
#include <QDebug>
#include <QtConcurrent>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.MediaProperties.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Devices.h>
#include <MemoryBuffer.h>

// 引入必要的命名空间，解决 C2065 错误
using namespace winrt::Windows::Media::MediaProperties;
using namespace winrt::Windows::Media::Capture;
using namespace winrt::Windows::Media::Devices;

struct __declspec(uuid("5b0d3235-4dba-4d44-865e-8f1d0e4fd04d")) __declspec(novtable) IMemoryBufferByteAccess : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetBuffer(uint8_t** value, uint32_t* capacity) = 0;
};

// IID 定义
static const GUID IID_IDirect3DDxgiInterfaceAccess_Local =
    { 0xA9B3D012, 0x3DF2, 0x4EE3, { 0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1 } };
struct IDirect3DDxgiInterfaceAccess_Local : public IUnknown
{ virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID iid, void **p) = 0; };

WinRTCamera::WinRTCamera(QObject *parent) : QObject(parent) {}
WinRTCamera::~WinRTCamera() { stop(); }

bool WinRTCamera::init(ID3D11Device* device) {
    if (!device) return false;
    m_d3dDevice = device;
    m_d3dDevice->GetImmediateContext(&m_d3dContext);

    ComPtr<IDXGIDevice> dxgiDevice;
    m_d3dDevice.As(&dxgiDevice);
    winrt::com_ptr<IInspectable> inspectable;
    CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put());
    if (inspectable) {
        m_winrtDevice = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
    }
    return true;
}

bool WinRTCamera::start(const QString& deviceId) {
    if (m_running) stop();
    m_currentDeviceId = deviceId;
    m_frameCount = 0;

    try {
        m_mediaCapture = MediaCapture();

        m_mediaCapture.Failed([this](MediaCapture const&, MediaCaptureFailedEventArgs const& args) {
            qWarning() << "[WinRTCamera] CRITICAL ERROR:" << (int)args.Code() << QString::fromStdWString(args.Message().c_str());
        });

        MediaCaptureInitializationSettings settings;
        settings.VideoDeviceId(deviceId.toStdWString());
        settings.MemoryPreference(MediaCaptureMemoryPreference::Cpu);
        settings.StreamingCaptureMode(StreamingCaptureMode::Video);
        settings.SharingMode(MediaCaptureSharingMode::SharedReadOnly);

        m_mediaCapture.InitializeAsync(settings).get();

        // --- 使用 VideoDeviceController 协商格式 ---
        auto deviceController = m_mediaCapture.VideoDeviceController();

        // 获取所有可用格式 (VideoRecord 流)
        auto allProperties = deviceController.GetAvailableMediaStreamProperties(MediaStreamType::VideoRecord);

        // 【修复】明确类型定义，防止未声明错误
        VideoEncodingProperties bestFormat = nullptr;
        int bestScore = -1;

        qDebug() << "=== Scanning Formats (Controller API) ===";

        for (auto prop : allProperties) {
            // 【修复】因为使用了 using namespace，这里可以直接转换
            auto videoProp = prop.as<VideoEncodingProperties>();
            if (!videoProp) continue;

            auto subtype = videoProp.Subtype();
            int w = videoProp.Width();
            int h = videoProp.Height();
            double fps = (double)videoProp.FrameRate().Numerator() / videoProp.FrameRate().Denominator();

            // 调试输出
            // qDebug() << "  Fmt:" << QString::fromStdWString(subtype.c_str()) << w << "x" << h << "@" << fps;

            int score = 0;

            // 1. 格式评分
            // 【修复】使用完整限定名确保比较正确
            bool isNV12 = (subtype == winrt::Windows::Media::MediaProperties::MediaEncodingSubtypes::Nv12());
            bool isYUY2 = (subtype == winrt::Windows::Media::MediaProperties::MediaEncodingSubtypes::Yuy2());
            bool isBgra = (subtype == winrt::Windows::Media::MediaProperties::MediaEncodingSubtypes::Bgra8());

            if (isNV12) score += 2000;
            else if (isYUY2) score += 1000;
            else if (isBgra) score += 500;
            else continue; // 跳过压缩格式

            // 2. 分辨率评分
            if (w == 1920 && h == 1080) score += 100;
            else if (w == 1280 && h == 720) score += 80;
            else if (w == 640 && h == 360) score += 50;

            // 3. 帧率评分
            if (std::abs(fps - 30.0) < 1.0) score += 50;
            if (std::abs(fps - 60.0) < 1.0) score += 60;

            if (score > bestScore) {
                bestScore = score;
                bestFormat = videoProp;
            }
        }
        qDebug() << "=======================================";

        if (bestFormat) {
            qDebug() << "[WinRTCamera] Setting Format:"
                     << QString::fromStdWString(bestFormat.Subtype().c_str())
                     << bestFormat.Width() << "x" << bestFormat.Height();

            // 设置格式
            deviceController.SetMediaStreamPropertiesAsync(MediaStreamType::VideoRecord, bestFormat).get();
        } else {
            qWarning() << "[WinRTCamera] NO UNCOMPRESSED FORMAT FOUND! Defaulting...";
        }

        // --- 绑定 FrameReader ---
        auto frameSources = m_mediaCapture.FrameSources();
        MediaFrameSource selectedSource = nullptr;
        for (auto pair : frameSources) {
            auto source = pair.Value();
            if (source.Info().SourceKind() == MediaFrameSourceKind::Color) { selectedSource = source; break; }
        }
        if (!selectedSource) { qDebug() << "[WinRTCamera] No Color Source"; return false; }

        m_frameReader = m_mediaCapture.CreateFrameReaderAsync(selectedSource).get();

        m_frameToken = m_frameReader.FrameArrived({ this, &WinRTCamera::onFrameArrived });
        MediaFrameReaderStartStatus status = m_frameReader.StartAsync().get();

        if (status == MediaFrameReaderStartStatus::Success) {
            m_running = true;
            qDebug() << "[WinRTCamera] Start SUCCESS.";
            return true;
        } else {
            qWarning() << "[WinRTCamera] Start Failed Status:" << (int)status;
        }

    } catch (winrt::hresult_error const& ex) {
        qWarning() << "[WinRTCamera] Exception:" << (int)ex.code() << QString::fromStdWString(ex.message().c_str());
    } catch (...) {
        qWarning() << "[WinRTCamera] Unknown Exception";
    }
    return false;
}

void WinRTCamera::stop() {
    m_running = false;
    try {
        if (m_frameReader) {
            m_frameReader.FrameArrived(m_frameToken);
            m_frameReader.StopAsync();
            m_frameReader.Close();
            m_frameReader = nullptr;
        }
        if (m_mediaCapture) { m_mediaCapture.Close(); m_mediaCapture = nullptr; }
    } catch (...) {}

    QMutexLocker locker(&m_mutex);
    m_currentSRV.Reset(); m_copyTexture.Reset();
}

void WinRTCamera::onFrameArrived(MediaFrameReader const& sender, MediaFrameArrivedEventArgs const&) {
    if (!m_running) return;

    // 调试：只打印前几帧
    int c = ++m_frameCount;
    if (c <= 5) qDebug() << "[WinRTCamera] Frame Arrived #" << c;

    try {
        auto frameRef = sender.TryAcquireLatestFrame();
        if (!frameRef) return;
        auto videoFrame = frameRef.VideoMediaFrame();
        if (!videoFrame) return;

        auto softwareBitmap = videoFrame.SoftwareBitmap();
        if (softwareBitmap) {
            auto buffer = softwareBitmap.LockBuffer(winrt::Windows::Graphics::Imaging::BitmapBufferAccessMode::Read);
            auto reference = buffer.CreateReference();
            auto byteAccess = reference.as<IMemoryBufferByteAccess>();

            uint8_t* pData = nullptr;
            uint32_t capacity = 0;
            if (SUCCEEDED(byteAccess->GetBuffer(&pData, &capacity)) && pData) {

                int w = softwareBitmap.PixelWidth();
                int h = softwareBitmap.PixelHeight();
                auto fmt = softwareBitmap.BitmapPixelFormat();

                if (c == 1) qDebug() << "[WinRTCamera] Bitmap:" << w << "x" << h << " Fmt:" << (int)fmt;

                QMutexLocker locker(&m_mutex);

                // 1. 初始化纹理
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

                    if (fmt == winrt::Windows::Graphics::Imaging::BitmapPixelFormat::Nv12) desc.Format = DXGI_FORMAT_NV12;
                    else if (fmt == winrt::Windows::Graphics::Imaging::BitmapPixelFormat::Yuy2) desc.Format = DXGI_FORMAT_YUY2;
                    else if (fmt == winrt::Windows::Graphics::Imaging::BitmapPixelFormat::Bgra8) desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    else desc.Format = DXGI_FORMAT_R8_UNORM;

                    if (FAILED(m_d3dDevice->CreateTexture2D(&desc, nullptr, m_copyTexture.ReleaseAndGetAddressOf()))) return;

                    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                    srvDesc.Texture2D.MipLevels = 1;
                    srvDesc.Texture2D.MostDetailedMip = 0;

                    // 对 NV12/YUY2 先只读 Y 通道验证数据
                    if (desc.Format == DXGI_FORMAT_NV12) srvDesc.Format = DXGI_FORMAT_R8_UNORM;
                    else if (desc.Format == DXGI_FORMAT_YUY2) srvDesc.Format = DXGI_FORMAT_R8G8_B8G8_UNORM;
                    else srvDesc.Format = desc.Format;

                    m_d3dDevice->CreateShaderResourceView(m_copyTexture.Get(), &srvDesc, m_currentSRV.ReleaseAndGetAddressOf());
                }

                // 2. 上传数据
                int rowPitch = w;
                if (fmt == winrt::Windows::Graphics::Imaging::BitmapPixelFormat::Bgra8) rowPitch = w * 4;
                if (fmt == winrt::Windows::Graphics::Imaging::BitmapPixelFormat::Yuy2) rowPitch = w * 2;

                m_d3dContext->UpdateSubresource(m_copyTexture.Get(), 0, nullptr, pData, rowPitch, 0);
            }
        }
    } catch (...) {}
}

ComPtr<ID3D11ShaderResourceView> WinRTCamera::getLatestFrame() {
    QMutexLocker locker(&m_mutex);
    return m_currentSRV;
}
