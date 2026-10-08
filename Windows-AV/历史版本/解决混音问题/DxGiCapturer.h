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
#include <wrl/client.h>
#include <atomic>
#include <QElapsedTimer>

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

    void setPreviewEnabled(bool enabled);
    void startRecording();
    void stopRecording();

    void addOverlay(int id, const QImage& img, const QRect& rect);
    void updateOverlayImage(int id, const QImage& img);
    void updateOverlayGeometry(int id, const QRect& rect);
    void removeOverlay(int id);
    void requestSnapshot();

public slots:
    void startCapture();
    void onUiFrameProcessed();
    void onRecorderFrameProcessed();

signals:
    void frameCaptured(const QImage &img, qint64 timestamp);
    void frameCapturedForPreview(const QImage &img);
    void snapshotCaptured(const QImage &img);
    void captureStopped();
    void errorOccurred(QString msg);

private:
    void cleanup();
    void resetCaptureState();
    bool initDevice(IDXGIAdapter* specificAdapter = nullptr);
    bool initShaders();
    bool initDuplication(int monitorIndex, ComPtr<IDXGIOutputDuplication>& outDupl);

    void renderTexture(ID3D11ShaderResourceView* srv, const QRect& rect, bool isBackground = false);
    void processPendingUpdates();
    void updateTextureFromImage(OverlayData* data);

    // 【新增】高精度睡眠函数
    void preciseSleep(double targetSeconds);

private:
    QAtomicInt m_running;
    QAtomicInt m_snapshotRequested;

    std::atomic<bool> m_previewEnabled;
    std::atomic<bool> m_isRecordingMP4;

    std::atomic<int> m_uiInFlight;
    std::atomic<int> m_recorderInFlight;

    QMutex m_targetMutex;
    CaptureTarget m_currentTarget;
    CaptureTarget m_pendingTarget;
    bool m_targetChanged = false;

    WgcCapturer* m_wgcCapturer = nullptr;

    ComPtr<ID3D11Device> m_d3dDevice;
    ComPtr<ID3D11DeviceContext> m_d3dContext;
    ComPtr<IDXGIOutputDuplication> m_deskDupl;

    ComPtr<ID3D11Texture2D> m_currentMonitorTexture;
    ComPtr<ID3D11Texture2D> m_compositionTexture;
    ComPtr<ID3D11RenderTargetView> m_compositionRTV;
    ComPtr<ID3D11Texture2D> m_stagingTexture;
    ComPtr<ID3D11ShaderResourceView> m_cachedSRV;
    ComPtr<ID3D11Texture2D> m_capturedTexture;

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

    // 计时与统计
    QElapsedTimer m_timestampTimer;   // 录制开始后的总时间
    QElapsedTimer m_loopTimer;        // 用于控制帧率循环
    QElapsedTimer m_logTimer;         // 用于每秒输出日志
    std::atomic<int> m_fpsCounter;    // 统计每秒采集帧数
};
#endif
