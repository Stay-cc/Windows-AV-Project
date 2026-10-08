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
    return list;
}

bool WasapiCapturer::initCapture() {
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&m_enumerator);
    if (FAILED(hr)) return false;

    if (m_deviceId.isEmpty()) {
        EDataFlow flow = (m_type == Microphone) ? eCapture : eRender;
        hr = m_enumerator->GetDefaultAudioEndpoint(flow, eConsole, &m_device);
    } else {
        hr = m_enumerator->GetDevice(m_deviceId.toStdWString().c_str(), &m_device);
    }
    if (FAILED(hr)) return false;

    hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_audioClient);
    if (FAILED(hr)) return false;

    hr = m_audioClient->GetMixFormat(&m_mixFormat);
    if (FAILED(hr)) return false;

    // 对于 Loopback，我们需要修改格式以匹配
    if (m_type == SystemLoopback) {
        // 通常 Loopback 使用系统混合格式，不需要强制修改，但需注意掩码
    }

    DWORD flags = (m_type == SystemLoopback) ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;

    // 初始化 Audio Client (Event Driven 模式可选，这里使用轮询简化逻辑)
    hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                                   REFTIMES_PER_SEC, 0, m_mixFormat, nullptr);
    if (FAILED(hr)) return false;

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

    // 计算每帧字节数
    int bytesPerFrame = m_mixFormat->nChannels * (m_mixFormat->wBitsPerSample / 8);

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
                    // 1. 处理数据用于录制
                    // 只有非静音数据才复制，或者如果是 AUDCLNT_BUFFERFLAGS_SILENT，则填零
                    QByteArray pcmData;
                    int dataSize = numFramesAvailable * bytesPerFrame;

                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                        pcmData.resize(dataSize);
                        pcmData.fill(0);
                    } else {
                        pcmData = QByteArray((const char*)pData, dataSize);
                    }

                    // 发送原始数据
                    emit audioDataReady(pcmData, m_mixFormat->nSamplesPerSec, m_mixFormat->nChannels);

                    // 2. 计算音量 (RMS) 用于 UI 显示
                    // 假设是 Float 32bit (WASAPI 共享模式默认通常是这个)
                    if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && m_mixFormat->wBitsPerSample == 32 && m_mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
                        float *ptr = (float*)pData;
                        double sum = 0;
                        for(UINT i=0; i<numFramesAvailable * m_mixFormat->nChannels; ++i) {
                            sum += ptr[i] * ptr[i];
                        }
                        float rms = sqrt(sum / (numFramesAvailable * m_mixFormat->nChannels));
                        emit volumeLevelChanged(rms);
                    } else if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && m_mixFormat->wBitsPerSample == 16) {
                        // 16bit int 处理
                        short *ptr = (short*)pData;
                        double sum = 0;
                        for(UINT i=0; i<numFramesAvailable * m_mixFormat->nChannels; ++i) {
                            double s = ptr[i] / 32768.0;
                            sum += s * s;
                        }
                        float rms = sqrt(sum / (numFramesAvailable * m_mixFormat->nChannels));
                        emit volumeLevelChanged(rms);
                    }
                }
                m_captureClient->ReleaseBuffer(numFramesAvailable);
            }
        } else {
            // 没有数据，稍作休眠避免空转
            QThread::msleep(10);
        }
    }

    m_audioClient->Stop();
    cleanup();
    CoUninitialize();
}
