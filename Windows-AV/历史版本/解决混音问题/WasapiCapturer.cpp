#include "WasapiCapturer.h"
#include <functiondiscoverykeys_devpkey.h>
#include <QDebug>
#include <cmath>

#define REFTIMES_PER_SEC  10000000
#define REFTIMES_PER_MILLISEC  10000

WasapiCapturer::WasapiCapturer(QObject *parent)
    : QThread(parent), m_stopRequested(false), m_type(Microphone)
{
}

WasapiCapturer::~WasapiCapturer() {
    stop();
    wait();
    cleanup();
}

void WasapiCapturer::stop() {
    m_stopRequested = true;
}

void WasapiCapturer::setDevice(const QString &deviceId, DeviceType type) {
    m_deviceId = deviceId;
    m_type = type;
}

void WasapiCapturer::cleanup() {
    if (m_mixFormat) {
        CoTaskMemFree(m_mixFormat);
        m_mixFormat = nullptr;
    }
    m_captureClient.Reset();
    m_audioClient.Reset();
    m_device.Reset();
    m_enumerator.Reset();
}

QList<AudioDeviceInfo> WasapiCapturer::getAvailableDevices(DeviceType type) {
    QList<AudioDeviceInfo> list;
    HRESULT hr = CoInitialize(nullptr);

    ComPtr<IMMDeviceEnumerator> enumerator;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr)) return list;

    EDataFlow dataFlow = (type == Microphone) ? eCapture : eRender;
    ComPtr<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(dataFlow, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) return list;

    UINT count;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; i++) {
        ComPtr<IMMDevice> device;
        collection->Item(i, &device);
        LPWSTR id = nullptr;
        device->GetId(&id);

        ComPtr<IPropertyStore> props;
        device->OpenPropertyStore(STGM_READ, &props);
        PROPVARIANT varName;
        PropVariantInit(&varName);
        props->GetValue(PKEY_Device_FriendlyName, &varName);

        AudioDeviceInfo info;
        info.id = QString::fromWCharArray(id);
        info.name = QString::fromWCharArray(varName.pwszVal);
        list.append(info);

        CoTaskMemFree(id);
        PropVariantClear(&varName);
    }
    // CoUninitialize(); // 注意：多线程下频繁调这个可能导致问题，Qt通常在主线程初始化 COM
    return list;
}

bool WasapiCapturer::initCapture() {
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&m_enumerator);
    if (FAILED(hr)) { qDebug() << "WASAPI: Failed to create enumerator"; return false; }

    if (m_deviceId.isEmpty()) {
        EDataFlow flow = (m_type == Microphone) ? eCapture : eRender;
        hr = m_enumerator->GetDefaultAudioEndpoint(flow, eConsole, &m_device);
        qDebug() << "WASAPI: Using Default Device for" << (m_type==Microphone?"Mic":"Sys");
    } else {
        hr = m_enumerator->GetDevice(m_deviceId.toStdWString().c_str(), &m_device);
        qDebug() << "WASAPI: Using Specific Device ID:" << m_deviceId;
    }
    if (FAILED(hr)) { qDebug() << "WASAPI: Failed to get device"; return false; }

    hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_audioClient);
    if (FAILED(hr)) return false;

    hr = m_audioClient->GetMixFormat(&m_mixFormat);
    if (FAILED(hr)) return false;

    // 强制尝试使用 Float 格式 (通常系统默认就是 Float，但为了保险)
    if (m_mixFormat->wFormatTag != WAVE_FORMAT_IEEE_FLOAT && m_mixFormat->wFormatTag != WAVE_FORMAT_EXTENSIBLE) {
        // 如果不是 float，这里可以考虑手动修改 mixFormat 请求，但 WASAPI Shared Mode 可能拒绝
        // 我们在 run 里面做格式转换更稳妥
    }

    DWORD flags = (m_type == SystemLoopback) ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;

    // 初始化 Audio Client
    hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                                   REFTIMES_PER_SEC, 0, m_mixFormat, nullptr);
    if (FAILED(hr)) {
        qDebug() << "WASAPI: Initialize failed. HR:" << hr;
        return false;
    }

    hr = m_audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&m_captureClient);
    if (FAILED(hr)) return false;

    return true;
}

void WasapiCapturer::run() {
    CoInitialize(nullptr);

    if (!initCapture()) {
        emit errorOccurred("Failed to initialize WASAPI");
        cleanup();
        CoUninitialize();
        return;
    }

    m_audioClient->Start();
    m_stopRequested = false;
    qDebug() << "[Wasapi] Capture Started. Device:" << (m_type==Microphone?"Mic":"Sys")
             << "Channels:" << m_mixFormat->nChannels
             << "Rate:" << m_mixFormat->nSamplesPerSec;

    while (!m_stopRequested) {
        UINT32 packetLength = 0;
        HRESULT hr = m_captureClient->GetNextPacketSize(&packetLength);

        if (SUCCEEDED(hr) && packetLength > 0) {
            BYTE *pData;
            UINT32 numFramesAvailable;
            DWORD flags;

            hr = m_captureClient->GetBuffer(&pData, &numFramesAvailable, &flags, nullptr, nullptr);

            if (SUCCEEDED(hr)) {
                if (numFramesAvailable > 0) {

                    // 统一转换为 Float32 格式发送给 Recorder
                    QByteArray floatData;
                    floatData.resize(numFramesAvailable * m_mixFormat->nChannels * sizeof(float));
                    float* outPtr = (float*)floatData.data();

                    bool isSilent = (flags & AUDCLNT_BUFFERFLAGS_SILENT);

                    // 格式转换逻辑
                    if (m_mixFormat->wBitsPerSample == 32 && (m_mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || m_mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE)) {
                        if (isSilent) memset(outPtr, 0, floatData.size());
                        else memcpy(outPtr, pData, floatData.size());
                    }
                    else if (m_mixFormat->wBitsPerSample == 16) {
                        short* inPtr = (short*)pData;
                        int totalSamples = numFramesAvailable * m_mixFormat->nChannels;
                        for (int i = 0; i < totalSamples; ++i) {
                            if (isSilent) outPtr[i] = 0.0f;
                            else outPtr[i] = inPtr[i] / 32768.0f;
                        }
                    }
                    else {
                        // 不支持的格式填静音，防止奔溃
                        memset(outPtr, 0, floatData.size());
                    }

                    // 计算 RMS 音量 (用于 UI 条跳动)
                    double sum = 0;
                    int totalSamples = numFramesAvailable * m_mixFormat->nChannels;
                    for(int i=0; i<totalSamples; ++i) {
                        sum += outPtr[i] * outPtr[i];
                    }
                    float rms = (totalSamples > 0) ? sqrt(sum / totalSamples) : 0.0f;

                    emit volumeLevelChanged(rms);

                    // 发送数据
                    emit audioDataReady(floatData, m_mixFormat->nSamplesPerSec, m_mixFormat->nChannels);
                }
                m_captureClient->ReleaseBuffer(numFramesAvailable);
            }
        } else {
            // 系统声音如果完全静音，Loopback 模式可能不会产生包
            // 休眠一下避免 CPU 100%
            QThread::msleep(5);
        }
    }

    m_audioClient->Stop();
    cleanup();
    CoUninitialize();
    qDebug() << "[Wasapi] Stopped.";
}
