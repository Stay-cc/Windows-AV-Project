#ifndef DXGICAPTURER_H
#define DXGICAPTURER_H

#include <QObject>
#include <QImage>
#include <QAtomicInt>
#include <QStringList>
#include <QMutex>
#include <QMap>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include "WgcCapturer.h"

// 引入 WRL ComPtr 智能指针
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

enum class CaptureSourceType { Monitor, Window };

struct CaptureTarget {
    CaptureSourceType type = CaptureSourceType::Monitor;
    int monitorIndex = 0;
    HWND windowHandle = nullptr;

    bool operator==(const CaptureTarget& other) const {
        if (type != other.type) return false;
        if (type == CaptureSourceType::Monitor) return monitorIndex == other.monitorIndex;
        return windowHandle == other.windowHandle;
    }
    bool operator!=(const CaptureTarget& other) const { return !(*this == other); }
};

struct OverlayData {
    int id;
    QImage image;
    QRect rect;
    bool textureDirty;
    bool rectDirty;
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
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
    void setCaptureTarget(const CaptureTarget& target);
    void stopCapture();

    void addOverlay(int id, const QImage& img, const QRect& rect);
    void updateOverlayImage(int id, const QImage& img);
    void updateOverlayGeometry(int id, const QRect& rect);
    void removeOverlay(int id);
    void requestSnapshot();

public slots:
    void startCapture();

signals:
    void frameCaptured(QImage img);
    void snapshotCaptured(QImage img);
    void captureStopped();
    void errorOccurred(QString msg);

private:
    void cleanup();
    void resetCaptureState(); // 新增：重置采集状态
    bool initDevice(IDXGIAdapter* specificAdapter = nullptr);
    bool initShaders();
    bool initDuplication(int monitorIndex, ComPtr<IDXGIOutputDuplication>& outDupl);

    void renderTexture(ID3D11ShaderResourceView* srv, const QRect& rect, bool isBackground = false);
    void processPendingUpdates();
    void updateTextureFromImage(OverlayData* data);

private:
    QAtomicInt m_running;
    QAtomicInt m_snapshotRequested;

    QMutex m_targetMutex;
    CaptureTarget m_currentTarget;
    CaptureTarget m_pendingTarget;
    bool m_targetChanged = false;

    // 当前活跃的 WGC 采集器
    WgcCapturer* m_wgcCapturer = nullptr;

    // D3D11 资源 (使用 ComPtr 自动管理)
    ComPtr<ID3D11Device> m_d3dDevice;
    ComPtr<ID3D11DeviceContext> m_d3dContext;

    // DXGI 采集对象
    ComPtr<IDXGIOutputDuplication> m_deskDupl;

    // 纹理缓存
    ComPtr<ID3D11Texture2D> m_currentMonitorTexture;
    ComPtr<ID3D11Texture2D> m_compositionTexture;
    ComPtr<ID3D11RenderTargetView> m_compositionRTV;
    ComPtr<ID3D11Texture2D> m_stagingTexture;
    ComPtr<ID3D11ShaderResourceView> m_cachedSRV; // 提升为成员变量，防止局部变量丢失
    ComPtr<ID3D11Texture2D> m_capturedTexture;    // 提升为成员变量

    // Shader 资源
    ComPtr<ID3D11VertexShader> m_vertexShader;
    ComPtr<ID3D11PixelShader> m_pixelShader;
    ComPtr<ID3D11InputLayout> m_inputLayout;
    ComPtr<ID3D11Buffer> m_vertexBuffer;
    ComPtr<ID3D11SamplerState> m_samplerState;
    ComPtr<ID3D11BlendState> m_blendState;
    ComPtr<ID3D11RasterizerState> m_rasterizerState;
    ComPtr<ID3D11DepthStencilState> m_depthStencilState;

    int m_width = 0;
    int m_height = 0;

    QMutex m_dataMutex;
    QMap<int, OverlayData*> m_overlays;
    QList<int> m_pendingRemovals;
};
#endif
