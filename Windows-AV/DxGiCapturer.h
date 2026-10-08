#ifndef DXGICAPTURER_H
#define DXGICAPTURER_H

#include <QObject>
#include <QImage>
#include <QMutex>
#include <QMap>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include "WgcCapturer.h"
#include "WinRTCamera.h"

using Microsoft::WRL::ComPtr;

enum class CaptureSourceType { Monitor, Window };

struct CaptureTarget {
    CaptureSourceType type = CaptureSourceType::Monitor;
    int monitorIndex = 0;
    HWND windowHandle = nullptr;
    bool operator!=(const CaptureTarget& other) const {
        return type != other.type || monitorIndex != other.monitorIndex || windowHandle != other.windowHandle;
    }
};

enum class OverlayType { Image, Camera };

struct OverlayData {
    int id = 0;
    OverlayType type = OverlayType::Image;
    QImage image; // 存储图片或文字的像素数据
    WinRTCamera* camera = nullptr;// 摄像头图层的数据源
    QRect rect;  // 图层在屏幕上的位置和大小
    bool textureDirty = false; // 标记是否需要上传到GPU
    ID3D11Texture2D* texture = nullptr;// GPU 端纹理
    ID3D11ShaderResourceView* srv = nullptr;// 纹理的 SRV（着色器读取视图）
};

class DxGiCapturer : public QObject
{
    Q_OBJECT
public:
    explicit DxGiCapturer(QObject *parent = nullptr);
    ~DxGiCapturer();

    static QStringList getMonitorNames();
    static QMap<size_t, QString> getWindowList();

    bool init();
    ID3D11Device* getD3DDevice() const { return m_d3dDevice.Get(); }

    void setCaptureTarget(const CaptureTarget& target);
    void setPreviewEnabled(bool enabled);

    // 在同一个线程调用时可以直接使用，但为了安全建议都通过 Slot 调用
    void addOverlay(int id, const QImage& img, const QRect& rect);
    void addCameraOverlay(int id, WinRTCamera* camera, const QRect& rect);

public slots:
    // --- 【核心修复】必须声明为 Slots 才能被 SystemController invokeMethod 调用 ---
    void updateRenderOrder(QList<int> order);
    void updateOverlayImage(int id, const QImage& img);
    void updateOverlayGeometry(int id, const QRect& rect);
    void removeOverlay(int id);

    void requestSnapshot();
    void stopCapture();
    void startCapture();
    void onUiFrameProcessed();
    void startRecording();
    void stopRecording();

signals:
    void initFinished(bool success);
    void frameCaptured(const QImage &img, qint64 timestamp);
    void frameCapturedForPreview(const QImage &img);
    void snapshotCaptured(const QImage &img);
    void captureStopped();
    void errorOccurred(QString msg);

private:
    void cleanup();
    void resetCaptureState();
    bool initDevice(IDXGIAdapter* adapter = nullptr);
    bool initShaders();
    bool initDuplication(int monitorIndex, ComPtr<IDXGIOutputDuplication>& outDupl);
    void renderTexture(ID3D11ShaderResourceView* srv, const QRect& rect, bool isBackground = false);
    void processPendingUpdates();
    void updateTextureFromImage(OverlayData* data);
    void preciseSleep(double seconds);

private:
    std::atomic<bool> m_running{false};
    std::atomic<int> m_snapshotRequested{0};
    std::atomic<bool> m_previewEnabled{true};
    std::atomic<bool> m_isRecordingMP4{false};
    std::atomic<int> m_uiInFlight{0};

    QMutex m_targetMutex;
    CaptureTarget m_currentTarget;
    CaptureTarget m_pendingTarget;
    bool m_targetChanged = false;

    WgcCapturer* m_wgcCapturer = nullptr;

    ComPtr<ID3D11Device> m_d3dDevice;
    ComPtr<ID3D11DeviceContext> m_d3dContext;
    ComPtr<IDXGIOutputDuplication> m_deskDupl;
    LUID m_currentAdapterLuid = { 0, 0 };

    // Resources
    ComPtr<ID3D11Texture2D> m_currentMonitorTexture;
    ComPtr<ID3D11Texture2D> m_compositionTexture;
    ComPtr<ID3D11RenderTargetView> m_compositionRTV;
    ComPtr<ID3D11Texture2D> m_stagingTexture;
    ComPtr<ID3D11ShaderResourceView> m_cachedSRV;
    ComPtr<ID3D11Texture2D> m_capturedTexture;

    // Shaders
    ComPtr<ID3D11VertexShader> m_vertexShader;
    ComPtr<ID3D11PixelShader> m_pixelShader;
    ComPtr<ID3D11InputLayout> m_inputLayout;
    ComPtr<ID3D11Buffer> m_vertexBuffer;
    ComPtr<ID3D11SamplerState> m_samplerState;
    ComPtr<ID3D11BlendState> m_blendState;
    ComPtr<ID3D11RasterizerState> m_rasterizerState;

    int m_width = 0; int m_height = 0;
    QMutex m_dataMutex;
    QMap<int, OverlayData*> m_overlays;
    QList<int> m_pendingRemovals;
    QList<int> m_renderOrder; // 渲染顺序 ID 列表
};

#endif // DXGICAPTURER_H
