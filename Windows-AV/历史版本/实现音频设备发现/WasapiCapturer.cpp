#include "WasapiCapturer.h"
#include <QDebug>
#include <functiondiscoverykeys_devpkey.h>
#include <cmath>
#include <algorithm> // 引入 std::min

// 宏定义用于释放 COM 对象
#define SAFE_RELEASE(punk)  \
if ((punk) != NULL)  \
    { (punk)->Release(); (punk) = NULL; }

const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
const IID IID_IAudioClient = __uuidof(IAudioClient);
const IID IID_IAudioCaptureClient = __uuidof(IAudioCaptureClient);

WasapiCapturer::WasapiCapturer(QObject *parent)
    : QThread(parent), m_running(false), m_captureType(SystemLoopback)
{
}

WasapiCapturer::~WasapiCapturer() {
    stop();
    wait();
    cleanup();
}

void WasapiCapturer::setDevice(const QString &deviceId, CaptureType type) {
    // 【关键修复】使用成员变量 m_mutex，禁止对临时对象取地址
    QMutexLocker locker(&m_mutex);
    m_targetDeviceId = deviceId;
    m_captureType = type;
}

void WasapiCapturer::stop() {
    m_running = false;
}

void WasapiCapturer::cleanup() {
    if (m_pwfx) { CoTaskMemFree(m_pwfx); m_pwfx = NULL; }
    SAFE_RELEASE(m_captureClient);
    SAFE_RELEASE(m_audioClient);
    SAFE_RELEASE(m_device);
    SAFE_RELEASE(m_enumerator);
}

// 静态方法：枚举设备
QList<AudioDeviceInfo> WasapiCapturer::getAvailableDevices(CaptureType type) {
    QList<AudioDeviceInfo> devices;
    HRESULT hr;

    // 需要在一个线程中初始化 COM，这里临时初始化
    CoInitialize(NULL);

    IMMDeviceEnumerator *pEnumerator = NULL;
    hr = CoCreateInstance(CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)&pEnumerator);
    if (FAILED(hr)) { CoUninitialize(); return devices; }

    IMMDeviceCollection *pCollection = NULL;
    // 如果是内录(Loopback)，我们需要枚举 "渲染(Render)" 设备(扬声器)
    // 如果是麦克风，我们需要枚举 "捕获(Capture)" 设备
    EDataFlow dataFlow = (type == SystemLoopback) ? eRender : eCapture;

    hr = pEnumerator->EnumAudioEndpoints(dataFlow, DEVICE_STATE_ACTIVE, &pCollection);
    if (FAILED(hr)) { SAFE_RELEASE(pEnumerator); CoUninitialize(); return devices; }

    UINT count;
    pCollection->GetCount(&count);

    for (UINT i = 0; i < count; i++) {
        IMMDevice *pEndpoint = NULL;
        pCollection->Item(i, &pEndpoint);

        LPWSTR pwszID = NULL;
        pEndpoint->GetId(&pwszID);

        IPropertyStore *pProps = NULL;
        pEndpoint->OpenPropertyStore(STGM_READ, &pProps);

        PROPVARIANT varName;
        PropVariantInit(&varName);
        pProps->GetValue(PKEY_Device_FriendlyName, &varName);

        AudioDeviceInfo info;
        info.id = QString::fromWCharArray(pwszID);
        info.name = QString::fromWCharArray(varName.pwszVal);
        info.isLoopback = (type == SystemLoopback);
        devices.append(info);

        CoTaskMemFree(pwszID);
        PropVariantClear(&varName);
        SAFE_RELEASE(pProps);
        SAFE_RELEASE(pEndpoint);
    }

    SAFE_RELEASE(pCollection);
    SAFE_RELEASE(pEnumerator);
    CoUninitialize();

    return devices;
}

HRESULT WasapiCapturer::initWasapi() {
    HRESULT hr;
    hr = CoCreateInstance(CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)&m_enumerator);
    if (FAILED(hr)) return hr;

    // 获取设备
    if (m_targetDeviceId.isEmpty()) {
        // 默认设备
        EDataFlow dataFlow = (m_captureType == SystemLoopback) ? eRender : eCapture;
        hr = m_enumerator->GetDefaultAudioEndpoint(dataFlow, eConsole, &m_device);
    } else {
        hr = m_enumerator->GetDevice((LPCWSTR)m_targetDeviceId.utf16(), &m_device);
    }
    if (FAILED(hr)) return hr;

    // 激活 AudioClient
    hr = m_device->Activate(IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&m_audioClient);
    if (FAILED(hr)) return hr;

    // 获取混合格式
    hr = m_audioClient->GetMixFormat(&m_pwfx);
    if (FAILED(hr)) return hr;

    // 初始化 AudioClient
    // 关键点：如果是系统内录，必须加上 AUDCLNT_STREAMFLAGS_LOOPBACK
    DWORD flags = 0;
    if (m_captureType == SystemLoopback) {
        flags = AUDCLNT_STREAMFLAGS_LOOPBACK;
    }

    // REFERENCE_TIME: 100ns units. 10ms = 100000
    hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 100000, 0, m_pwfx, NULL);
    if (FAILED(hr)) return hr;

    // 获取 CaptureClient
    hr = m_audioClient->GetService(IID_IAudioCaptureClient, (void**)&m_captureClient);
    return hr;
}

void WasapiCapturer::run() {
    // 线程内初始化 COM
    CoInitialize(NULL);

    if (FAILED(initWasapi())) {
        emit errorOccurred("Failed to initialize WASAPI");
        cleanup();
        CoUninitialize();
        return;
    }

    if (FAILED(m_audioClient->Start())) {
        emit errorOccurred("Failed to start audio client");
        cleanup();
        CoUninitialize();
        return;
    }

    m_running = true;
    UINT32 packetLength = 0;
    BYTE *pData;
    UINT32 numFramesAvailable;
    DWORD flags;

    while (m_running) {
        // 简单的休眠以模拟缓冲区填充
        QThread::msleep(10);

        HRESULT hr = m_captureClient->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) break;

        while (packetLength != 0) {
            hr = m_captureClient->GetBuffer(&pData, &numFramesAvailable, &flags, NULL, NULL);
            if (FAILED(hr)) break;

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                // 静音帧，数据全 0
                emit volumeLevelChanged(0.0f);
            } else {
                // 1. 发送原始数据
                QByteArray audioBuf((const char*)pData, numFramesAvailable * m_pwfx->nBlockAlign);
                emit audioDataReady(audioBuf, m_pwfx->nChannels, m_pwfx->nSamplesPerSec, m_pwfx->wBitsPerSample);

                // 2. 简单计算音量 (RMS) 用于 UI 显示
                // 这里只演示 32位 float 格式 (WASAPI Shared Mode 通常默认 float)
                if (m_pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                    (m_pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                     reinterpret_cast<WAVEFORMATEXTENSIBLE*>(m_pwfx)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {

                    float* floatData = (float*)pData;
                    float sum = 0;
                    int samples = numFramesAvailable * m_pwfx->nChannels;
                    for (int i=0; i<samples; ++i) {
                        float val = floatData[i];
                        sum += val * val;
                    }
                    if (samples > 0) {
                        float rms = std::sqrt(sum / samples);
                        // 稍微放大一点以便观察
                        // 因为加了 NOMINMAX，这里的 std::min 不会再报错了
                        emit volumeLevelChanged(std::min(1.0f, rms * 5.0f));
                    }
                }
            }

            hr = m_captureClient->ReleaseBuffer(numFramesAvailable);
            if (FAILED(hr)) break;

            hr = m_captureClient->GetNextPacketSize(&packetLength);
        }
    }

    m_audioClient->Stop();
    cleanup();
    CoUninitialize();
}
