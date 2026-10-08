#ifndef FFMPEGRECORDER_H
#define FFMPEGRECORDER_H

#include <QObject>
#include <QThread>
#include <QImage>
#include <QMutex>
#include <QQueue>
#include <QMap>
#include <atomic>
#include "RecorderConfig.h" // [引入配置]

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/time.h>
}

struct VideoFrameData {
    QImage image;
    qint64 timestamp;
};

struct AudioChunk {
    QByteArray data;
    int sampleRate;
    int channels;
    qint64 timestamp;
};

class FFmpegRecorder : public QThread
{
    Q_OBJECT
public:
    explicit FFmpegRecorder(QObject *parent = nullptr);
    ~FFmpegRecorder();

    // [修改] 启动时传入配置
    bool startRecord(const RecorderConfig& config);
    void stopRecord();

public slots:
    void pushVideoFrame(const QImage &img, qint64 timestamp);
    void pushAudioData(int sourceId, const QByteArray &data, int sampleRate, int channels);

signals:
    void frameProcessed();

protected:
    void run() override;

private:
    bool initOutput();
    bool initVideoEncoder();
    bool initAudioEncoder();

    void ensureAudioSource(int id);
    bool resampleToFifo(int sourceId, const QByteArray &data, int sampleRate, int channels);
    void processVideo(const VideoFrameData &frameData);
    void flushEncoders();
    void closeResources();

    // [新增] 音频效果处理算法
    void applyFadeEffect(float* bufferL, float* bufferR, int samples, qint64 totalEncodedSamples, bool isFadingOut, qint64 fadeOutStartSample);

private:
    RecorderConfig m_config; // [核心配置]

    std::atomic<bool> m_isRecording;
    std::atomic<bool> m_softStopRequested; // 软停止标记 (用于淡出)

    qint64 m_firstFrameTimestamp = -1;

    AVFormatContext *m_fmtCtx = nullptr;
    AVCodecContext *m_vCtx = nullptr;
    AVStream *m_vStream = nullptr;
    AVFrame *m_vFrame = nullptr;
    SwsContext *m_swsCtx = nullptr;

    AVCodecContext *m_aCtx = nullptr;
    AVStream *m_aStream = nullptr;
    AVFrame *m_aFrame = nullptr;
    qint64 m_totalAudioSamples = 0;

    struct SourceContext {
        SwrContext *swrCtx = nullptr;
        AVAudioFifo *fifo = nullptr;
        int inSampleRate = 0;
    };
    QMap<int, SourceContext> m_audioSources;

    QMutex m_videoMutex;
    QQueue<VideoFrameData> m_videoQueue;
    QMutex m_audioMutex;
    QQueue<QPair<int, AudioChunk>> m_audioQueue;
};

#endif // FFMPEGRECORDER_H
