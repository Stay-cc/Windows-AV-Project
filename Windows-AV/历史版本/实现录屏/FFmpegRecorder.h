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
    void pushVideoFrame(const QImage &img);
    void pushAudioData(int sourceId, const QByteArray &data, int sampleRate, int channels);

    // 【关键修复】必须在此处声明信号，否则CPP文件会报错 C3861
signals:
    void frameProcessed();

protected:
    void run() override;

private:
    bool initOutput(const QString &filename);
    bool initVideoEncoder(int width, int height);
    bool initAudioEncoder();

    void processVideo(const QImage &img);
    void processAudio();
    void checkAndPushSilence();

    void flushEncoders();
    void closeResources();
    bool resampleToFifo(int sourceId, const QByteArray &data, int sampleRate, int channels);

    void log(const QString& msg);

private:
    bool m_isRecording;
    bool m_stopRequested;
    QString m_filename;
    int m_width;
    int m_height;

    AVFormatContext *m_fmtCtx = nullptr;
    AVCodecContext *m_vCtx = nullptr;
    AVStream *m_vStream = nullptr;
    AVFrame *m_vFrame = nullptr;
    SwsContext *m_swsCtx = nullptr;

    AVCodecContext *m_aCtx = nullptr;
    AVStream *m_aStream = nullptr;
    AVFrame *m_aFrame = nullptr;
    qint64 m_nextAudioPts = 0;

    struct SourceContext {
        SwrContext *swrCtx = nullptr;
        AVAudioFifo *fifo = nullptr;
        int inSampleRate = 0;
        int inChannels = 0;
    };
    QMap<int, SourceContext> m_audioSources;

    QMutex m_videoMutex;
    QQueue<QImage> m_videoQueue;

    QMutex m_audioMutex;
    QQueue<QPair<int, AudioChunk>> m_audioQueue;

    QElapsedTimer m_timer;
};

#endif // FFMPEGRECORDER_H
