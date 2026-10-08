#ifndef WASAPICAPTURER_H
#define WASAPICAPTURER_H

// 【关键修复1】必须在包含 windows.h 之前定义 NOMINMAX，否则 std::min 会报错
#define NOMINMAX

#include <QThread>
#include <QMutex>
#include <QByteArray>
#include <QList>
#include <QString>
#include <QObject>

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

// 音频设备信息结构
struct AudioDeviceInfo {
    QString id;
    QString name;
    bool isLoopback; // true=系统声音(扬声器), false=麦克风
};

class WasapiCapturer : public QThread
{
    Q_OBJECT
public:
    enum CaptureType {
        Microphone,     // 麦克风输入
        SystemLoopback  // 系统声音内录
    };

    explicit WasapiCapturer(QObject *parent = nullptr);
    ~WasapiCapturer();

    // 静态方法：枚举设备
    static QList<AudioDeviceInfo> getAvailableDevices(CaptureType type);

    // 设置目标设备 ID (空字符串则使用默认设备)
    void setDevice(const QString &deviceId, CaptureType type);
    void stop();

signals:
    // 原始 PCM 数据信号 (数据, 通道数, 采样率, 位深)
    void audioDataReady(QByteArray data, int channels, int sampleRate, int bitsPerSample);

    // 音量电平信号 (0.0 - 1.0)，用于UI跳动显示
    void volumeLevelChanged(float level);

    void errorOccurred(QString msg);

protected:
    void run() override;

private:
    // WASAPI 辅助函数
    HRESULT initWasapi();
    void cleanup();

private:
    bool m_running;
    QString m_targetDeviceId;
    CaptureType m_captureType;

    // 【关键修复2】添加成员变量锁，而不是使用临时对象
    mutable QMutex m_mutex;

    // COM 指针
    IMMDeviceEnumerator *m_enumerator = nullptr;
    IMMDevice *m_device = nullptr;
    IAudioClient *m_audioClient = nullptr;
    IAudioCaptureClient *m_captureClient = nullptr;
    WAVEFORMATEX *m_pwfx = nullptr;
};

#endif // WASAPICAPTURER_H
