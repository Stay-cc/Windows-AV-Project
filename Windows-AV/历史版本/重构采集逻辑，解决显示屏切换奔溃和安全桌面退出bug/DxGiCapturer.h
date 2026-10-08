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
    bool init(int monitorIndex);
    void stopCapture();

    // 线程安全的接口
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

private:
    void cleanup();
    bool initShaders();
    // 【新增】初始化或重建 Duplication 接口
    bool initDuplication();
    void renderOverlay(ID3D11ShaderResourceView* srv, const QRect& rect);
    void processPendingUpdates();
    void updateTextureFromImage(OverlayData* data);

private:
    QAtomicInt m_running;
    QAtomicInt m_snapshotRequested;

    // 记录当前监控的索引，用于丢失后重建
    int m_currentMonitorIndex = 0;

    ID3D11Device* m_d3dDevice = nullptr;
    ID3D11DeviceContext* m_d3dContext = nullptr;
    IDXGIOutputDuplication* m_deskDupl = nullptr;

    ID3D11Texture2D* m_compositionTexture = nullptr;
    ID3D11RenderTargetView* m_compositionRTV = nullptr;
    ID3D11Texture2D* m_stagingTexture = nullptr;

    ID3D11VertexShader* m_vertexShader = nullptr;
    ID3D11PixelShader* m_pixelShader = nullptr;
    ID3D11InputLayout* m_inputLayout = nullptr;
    ID3D11Buffer* m_vertexBuffer = nullptr;
    ID3D11SamplerState* m_samplerState = nullptr;
    ID3D11BlendState* m_blendState = nullptr;
    ID3D11RasterizerState* m_rasterizerState = nullptr;
    ID3D11DepthStencilState* m_depthStencilState = nullptr;

    int m_width = 0;
    int m_height = 0;

    QMutex m_dataMutex;
    QMap<int, OverlayData*> m_overlays;
    QList<int> m_pendingRemovals;
};
#endif
