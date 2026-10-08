#ifndef FFMPEGRECORDER_H
#define FFMPEGRECORDER_H

#include <QObject>
#include <QThread>
#include <QImage>
#include <QMutex>
#include <QQueue>
#include <QElapsedTimer>
#include <QMap>
#include <QDebug>
#include <QDateTime>
#include <atomic>

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

    bool startRecord(const QString &filename, int width, int height);
    void stopRecord();

public slots:
    void pushVideoFrame(const QImage &img, qint64 timestamp);
    void pushAudioData(int sourceId, const QByteArray &data, int sampleRate, int channels);

signals:
    void frameProcessed();

protected:
    void run() override;

private:
    bool initOutput(const QString &filename);
    bool initVideoEncoder(int width, int height);
    bool initAudioEncoder();

    void processVideo(const VideoFrameData &frameData);
    void processAudio();

    void ensureAudioSource(int id);
    bool resampleToFifo(int sourceId, const QByteArray &data, int sampleRate, int channels);

    void flushEncoders();
    void closeResources();
    void log(const QString& msg);

    // [修复点] 新增函数声明
    bool hasAudioPending();
    void flushAudioFifo();

private:
    std::atomic<bool> m_isRecording;
    std::atomic<bool> m_stopRequested;
    QString m_filename;
    int m_width;
    int m_height;
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
        int inChannels = 0;
    };

    QMap<int, SourceContext> m_audioSources;

    QMutex m_videoMutex;
    QQueue<VideoFrameData> m_videoQueue;

    QMutex m_audioMutex;
    QQueue<QPair<int, AudioChunk>> m_audioQueue;

    // [新增] 标记是否已经处理过开头的淡入
    bool m_hasAppliedFadeIn = false;
};

#endif // FFMPEGRECORDER_H
