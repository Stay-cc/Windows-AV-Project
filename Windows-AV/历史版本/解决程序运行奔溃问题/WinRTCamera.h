#ifndef WINRTCAMERA_H
#define WINRTCAMERA_H

#include <d3d11.h>
#include <dxgi1_2.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Capture.h>
#include <winrt/Windows.Media.Capture.Frames.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <wrl/client.h>
#include <QObject>
#include <QMutex>
#include <atomic>
#include <QString>

using namespace Microsoft::WRL;
using namespace winrt::Windows::Media::Capture;
using namespace winrt::Windows::Media::Capture::Frames;

class WinRTCamera : public QObject
{
    Q_OBJECT
public:
    explicit WinRTCamera(QObject *parent = nullptr);
    ~WinRTCamera();

    bool init(ID3D11Device* device);
    bool start(const QString& deviceId);
    void stop();

    ComPtr<ID3D11ShaderResourceView> getLatestFrame();

    QString getDeviceId() const { return m_currentDeviceId; }
    bool isRunning() const { return m_running; }

private:
    void onFrameArrived(MediaFrameReader const& sender, MediaFrameArrivedEventArgs const& args);
    void cleanup();

private:
    QMutex m_mutex;
    QString m_currentDeviceId;
    std::atomic<bool> m_running{false};

    // D3D Resources
    ComPtr<ID3D11Device> m_d3dDevice;
    ComPtr<ID3D11DeviceContext> m_d3dContext;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_winrtDevice{ nullptr };

    // WinRT Resources
    MediaCapture m_mediaCapture{ nullptr };
    MediaFrameReader m_frameReader{ nullptr };
    winrt::event_token m_frameToken;

    // Buffer Resources (核心修复)
    ComPtr<ID3D11Texture2D> m_copyTexture;
    ComPtr<ID3D11Texture2D> m_stagingTexture;
    ComPtr<ID3D11ShaderResourceView> m_currentSRV;

    // 调试计数器
    std::atomic<int> m_frameCount{0};
};

#endif // WINRTCAMERA_H
