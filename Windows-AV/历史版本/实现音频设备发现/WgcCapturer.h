#ifndef WGCCAPTURER_H
#define WGCCAPTURER_H

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/base.h>
#include <mutex>
#include <atomic>

using namespace Microsoft::WRL;
using namespace winrt::Windows::Graphics::Capture;

class WgcCapturer
{
public:
    WgcCapturer();
    ~WgcCapturer();

    void init(ID3D11Device* device);
    bool startCapture(HWND hwnd);
    void stopCapture();
    bool acquireNextFrame(ID3D11Texture2D** outTexture);

private:
    ID3D11Device* m_d3dDevice = nullptr;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_winrtDevice{ nullptr };

    GraphicsCaptureItem m_item{ nullptr };
    Direct3D11CaptureFramePool m_framePool{ nullptr };
    GraphicsCaptureSession m_session{ nullptr };

    // WRL ComPtr
    ComPtr<ID3D11Texture2D> m_lastFrame;
    std::atomic<bool> m_hasNewFrame{ false };

    std::mutex m_mutex;
    winrt::event_token m_frameArrivedToken;
};

#endif // WGCCAPTURER_H
