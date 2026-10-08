#include "FFmpegRecorder.h"
#include <QDebug>
#include <QDateTime>
#include <QCoreApplication>

FFmpegRecorder::FFmpegRecorder(QObject *parent) : QThread(parent) {
    m_isRecording = false;
    m_stopRequested = false;
    this->moveToThread(this);
}

FFmpegRecorder::~FFmpegRecorder() {
    stopRecord();
    wait();
}

void FFmpegRecorder::log(const QString &msg) {
    qint64 time = QDateTime::currentMSecsSinceEpoch();
    qDebug() << QString("[%1][Recorder Thread %2] %3").arg(time).arg((quintptr)QThread::currentThreadId()).arg(msg);
}

bool FFmpegRecorder::startRecord(const QString &filename, int width, int height) {
    if (isRunning()) return false;
    m_filename = filename;
    m_width = (width % 2 == 0) ? width : width - 1;
    m_height = (height % 2 == 0) ? height : height - 1;

    { QMutexLocker l(&m_videoMutex); m_videoQueue.clear(); }
    { QMutexLocker l(&m_audioMutex); m_audioQueue.clear(); }

    m_stopRequested = false;
    start();
    return true;
}

void FFmpegRecorder::stopRecord() {
    if (isRunning()) {
        m_stopRequested = true;
    }
}

// 静态变量移到类成员或保持静态（注意多实例问题，这里简化保持静态）
static int s_droppedFrameCount = 0;

void FFmpegRecorder::pushVideoFrame(const QImage &img) {
    if (!m_isRecording) return;

    QMutexLocker locker(&m_videoMutex);

    // 【背压保护】
    if (m_videoQueue.size() > 5) {
        s_droppedFrameCount++;
        if (s_droppedFrameCount % 30 == 0) {
            log(QString("Queue full (%1). Dropping frame. Total Dropped: %2").arg(m_videoQueue.size()).arg(s_droppedFrameCount));
        }

        // 【核心修复】即使丢帧，也必须通知源头，否则源头的计数器无法减少，会导致源头死锁停止发送！
        locker.unlock(); // 解锁后再发信号
        emit frameProcessed();

        return;
    }

    m_videoQueue.enqueue(img.copy());
}

void FFmpegRecorder::pushAudioData(int sourceId, const QByteArray &data, int sampleRate, int channels) {
    if (!m_isRecording) return;
    QMutexLocker locker(&m_audioMutex);
    AudioChunk chunk;
    chunk.data = data;
    chunk.sampleRate = sampleRate;
    chunk.channels = channels;
    chunk.timestamp = m_timer.isValid() ? m_timer.elapsed() : 0;
    m_audioQueue.enqueue(qMakePair(sourceId, chunk));
}

void FFmpegRecorder::run() {
    log("Run loop started.");

    if (!initOutput(m_filename)) { log("Init Output Failed"); return; }
    if (!initVideoEncoder(m_width, m_height)) { log("Init Video Failed"); closeResources(); return; }
    if (!initAudioEncoder()) { log("Init Audio Failed"); closeResources(); return; }

    if (avformat_write_header(m_fmtCtx, nullptr) < 0) {
        log("Failed to write header");
        closeResources();
        return;
    }

    m_isRecording = true;
    m_timer.start();
    m_nextAudioPts = 0;
    s_droppedFrameCount = 0;

    log("Header written. Entering Event Loop...");

    while (!m_stopRequested) {
        QCoreApplication::processEvents();

        bool idle = true;

        // --- 视频处理 ---
        QImage img;
        bool hasVideo = false;
        {
            QMutexLocker locker(&m_videoMutex);
            if (!m_videoQueue.isEmpty()) {
                img = m_videoQueue.dequeue();
                hasVideo = true;
            }
        }
        if (hasVideo) {
            // 成功取出一帧，通知源头
            emit frameProcessed();

            processVideo(img);
            idle = false;
        }

        // --- 音频处理 ---
        while(true) {
            QPair<int, AudioChunk> chunk;
            bool hasAudio = false;
            {
                QMutexLocker locker(&m_audioMutex);
                if (!m_audioQueue.isEmpty()) {
                    chunk = m_audioQueue.dequeue();
                    hasAudio = true;
                }
            }
            if (hasAudio) {
                resampleToFifo(chunk.first, chunk.second.data, chunk.second.sampleRate, chunk.second.channels);
                idle = false;
            } else {
                break;
            }
        }

        checkAndPushSilence();
        processAudio();

        if (idle) msleep(1);
    }

    log("Stopping... Flushing encoders.");
    flushEncoders();
    av_write_trailer(m_fmtCtx);
    closeResources();
    m_isRecording = false;
    log("Recording Finished.");
}

void FFmpegRecorder::checkAndPushSilence() {
    if (!m_isRecording) return;
    double currentTime = m_timer.elapsed() / 1000.0;
    double audioTime = (double)m_nextAudioPts / m_aCtx->sample_rate;

    if (currentTime - audioTime > 0.2) {
        if (!m_audioSources.contains(0)) {
            QByteArray dummy;
            resampleToFifo(0, dummy, 44100, 2);
        }
        SourceContext &ctx = m_audioSources[0];
        if (ctx.fifo) {
            int frameSize = m_aCtx->frame_size;
            int currentFifoSize = av_audio_fifo_size(ctx.fifo);
            if (currentFifoSize < frameSize) {
                int fill = frameSize - currentFifoSize;
                float* silence = new float[fill * 2];
                memset(silence, 0, fill * 2 * sizeof(float));
                void* data[1] = { silence };
                av_audio_fifo_write(ctx.fifo, data, fill);
                delete[] silence;
            }
        }
    }
}

bool FFmpegRecorder::initOutput(const QString &filename) {
    log("Opening file: " + filename);
    int ret = avformat_alloc_output_context2(&m_fmtCtx, nullptr, nullptr, filename.toStdString().c_str());
    if (!m_fmtCtx) { log("Alloc context failed"); return false; }
    if (!(m_fmtCtx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&m_fmtCtx->pb, filename.toStdString().c_str(), AVIO_FLAG_WRITE) < 0) {
            log("Could not open file IO");
            return false;
        }
    }
    return true;
}

bool FFmpegRecorder::initVideoEncoder(int width, int height) {
    const char* encoderNames[] = { "h264_nvenc", "h264_qsv", "libx264", nullptr };
    const AVCodec *codec = nullptr;
    for (const char** name = encoderNames; *name != nullptr; name++) {
        codec = avcodec_find_encoder_by_name(*name);
        if (codec) { log(QString("Found Encoder: %1").arg(*name)); break; }
    }
    if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) return false;

    m_vStream = avformat_new_stream(m_fmtCtx, codec);
    m_vCtx = avcodec_alloc_context3(codec);
    m_vCtx->width = width; m_vCtx->height = height;
    m_vCtx->time_base = {1, 1000}; m_vCtx->framerate = {30, 1};
    m_vCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    if (QString(codec->name).contains("nvenc")) m_vCtx->pix_fmt = AV_PIX_FMT_NV12;
    m_vCtx->gop_size = 30; m_vCtx->bit_rate = 6000000;

    if (QString(codec->name) == "libx264") {
        av_opt_set(m_vCtx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(m_vCtx->priv_data, "tune", "zerolatency", 0);
    } else if (QString(codec->name).contains("nvenc")) {
        av_opt_set(m_vCtx->priv_data, "preset", "p1", 0);
        av_opt_set(m_vCtx->priv_data, "zerolatency", "1", 0);
    }

    if (m_fmtCtx->oformat->flags & AVFMT_GLOBALHEADER) m_vCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(m_vCtx, codec, nullptr) < 0) return false;
    avcodec_parameters_from_context(m_vStream->codecpar, m_vCtx);
    m_vFrame = av_frame_alloc();
    m_vFrame->width = width; m_vFrame->height = height;
    m_vFrame->format = m_vCtx->pix_fmt;
    av_frame_get_buffer(m_vFrame, 32);
    return true;
}

bool FFmpegRecorder::initAudioEncoder() {
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) return false;
    m_aStream = avformat_new_stream(m_fmtCtx, codec);
    m_aCtx = avcodec_alloc_context3(codec);
    m_aCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    m_aCtx->bit_rate = 128000;
    m_aCtx->sample_rate = 44100;
    AVChannelLayout layout; av_channel_layout_default(&layout, 2);
    av_channel_layout_copy(&m_aCtx->ch_layout, &layout);
    av_channel_layout_uninit(&layout);
    m_aCtx->time_base = {1, m_aCtx->sample_rate};
    if (m_fmtCtx->oformat->flags & AVFMT_GLOBALHEADER) m_aCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(m_aCtx, codec, nullptr) < 0) return false;
    avcodec_parameters_from_context(m_aStream->codecpar, m_aCtx);
    m_aFrame = av_frame_alloc();
    m_aFrame->nb_samples = m_aCtx->frame_size;
    m_aFrame->format = m_aCtx->sample_fmt;
    av_channel_layout_copy(&m_aFrame->ch_layout, &m_aCtx->ch_layout);
    m_aFrame->sample_rate = m_aCtx->sample_rate;
    av_frame_get_buffer(m_aFrame, 0);
    return true;
}

void FFmpegRecorder::processVideo(const QImage &img) {
    if (!m_swsCtx) {
        AVPixelFormat inputFmt = AV_PIX_FMT_BGRA;
        if (img.format() == QImage::Format_RGBA8888) inputFmt = AV_PIX_FMT_RGBA;
        m_swsCtx = sws_getContext(img.width(), img.height(), inputFmt, m_vCtx->width, m_vCtx->height, m_vCtx->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
    }
    const uint8_t *srcSlice[] = { img.constBits() };
    int srcStride[] = { (int)img.bytesPerLine() };
    sws_scale(m_swsCtx, srcSlice, srcStride, 0, img.height(), m_vFrame->data, m_vFrame->linesize);
    m_vFrame->pts = av_rescale_q(m_timer.elapsed(), {1, 1000}, m_vCtx->time_base);
    if (avcodec_send_frame(m_vCtx, m_vFrame) >= 0) {
        AVPacket *pkt = av_packet_alloc();
        while (avcodec_receive_packet(m_vCtx, pkt) >= 0) {
            av_packet_rescale_ts(pkt, m_vCtx->time_base, m_vStream->time_base);
            pkt->stream_index = m_vStream->index;
            av_interleaved_write_frame(m_fmtCtx, pkt);
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
    }
}

bool FFmpegRecorder::resampleToFifo(int sourceId, const QByteArray &data, int sampleRate, int channels) {
    if (!m_audioSources.contains(sourceId)) {
        SourceContext ctx;
        ctx.fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, 2, 1);
        m_audioSources.insert(sourceId, ctx);
    }
    SourceContext &ctx = m_audioSources[sourceId];
    if (data.isEmpty()) return true;
    if (!ctx.swrCtx || ctx.inSampleRate != sampleRate || ctx.inChannels != channels) {
        if (ctx.swrCtx) swr_free(&ctx.swrCtx);
        AVChannelLayout inLayout; av_channel_layout_default(&inLayout, channels);
        AVChannelLayout outLayout; av_channel_layout_copy(&outLayout, &m_aCtx->ch_layout);
        swr_alloc_set_opts2(&ctx.swrCtx, &outLayout, AV_SAMPLE_FMT_FLTP, m_aCtx->sample_rate, &inLayout, AV_SAMPLE_FMT_FLT, sampleRate, 0, nullptr);
        swr_init(ctx.swrCtx);
        ctx.inSampleRate = sampleRate; ctx.inChannels = channels;
        av_channel_layout_uninit(&inLayout); av_channel_layout_uninit(&outLayout);
    }
    int inputSamples = data.size() / (channels * sizeof(float));
    if (inputSamples <= 0) return false;
    int maxOutSamples = av_rescale_rnd(swr_get_delay(ctx.swrCtx, sampleRate) + inputSamples, m_aCtx->sample_rate, sampleRate, AV_ROUND_UP);
    uint8_t **outData = nullptr;
    av_samples_alloc_array_and_samples(&outData, nullptr, 2, maxOutSamples, AV_SAMPLE_FMT_FLTP, 0);
    const uint8_t *inData[] = { (const uint8_t*)data.constData() };
    int converted = swr_convert(ctx.swrCtx, outData, maxOutSamples, inData, inputSamples);
    if (converted > 0) {
        if (av_audio_fifo_space(ctx.fifo) < converted) av_audio_fifo_realloc(ctx.fifo, av_audio_fifo_size(ctx.fifo) + converted);
        av_audio_fifo_write(ctx.fifo, (void**)outData, converted);
    }
    if (outData) { av_freep(&outData[0]); av_freep(&outData); }
    return true;
}

void FFmpegRecorder::processAudio() {
    int frameSize = m_aCtx->frame_size;
    int maxAvail = 0;
    for(auto &ctx : m_audioSources) maxAvail = qMax(maxAvail, av_audio_fifo_size(ctx.fifo));
    if (maxAvail < frameSize) return;
    av_frame_make_writable(m_aFrame);
    float *dstL = (float*)m_aFrame->data[0];
    float *dstR = (float*)m_aFrame->data[1];
    memset(dstL, 0, frameSize * sizeof(float));
    memset(dstR, 0, frameSize * sizeof(float));
    float *tmpL = new float[frameSize]; float *tmpR = new float[frameSize];
    void *tmpData[2] = { tmpL, tmpR };
    for(auto &ctx : m_audioSources) {
        if (av_audio_fifo_size(ctx.fifo) >= frameSize) {
            av_audio_fifo_read(ctx.fifo, tmpData, frameSize);
            for(int i=0; i<frameSize; ++i) { dstL[i] += tmpL[i]; dstR[i] += tmpR[i]; }
        }
    }
    delete[] tmpL; delete[] tmpR;
    m_aFrame->pts = av_rescale_q(m_nextAudioPts, {1, m_aCtx->sample_rate}, m_aCtx->time_base);
    m_nextAudioPts += frameSize;
    if (avcodec_send_frame(m_aCtx, m_aFrame) >= 0) {
        AVPacket *pkt = av_packet_alloc();
        while (avcodec_receive_packet(m_aCtx, pkt) >= 0) {
            av_packet_rescale_ts(pkt, m_aCtx->time_base, m_aStream->time_base);
            pkt->stream_index = m_aStream->index;
            av_interleaved_write_frame(m_fmtCtx, pkt);
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
    }
}

void FFmpegRecorder::flushEncoders() {
    avcodec_send_frame(m_vCtx, nullptr);
    AVPacket *pkt = av_packet_alloc();
    while(avcodec_receive_packet(m_vCtx, pkt) >= 0) {
        av_packet_rescale_ts(pkt, m_vCtx->time_base, m_vStream->time_base);
        pkt->stream_index = m_vStream->index;
        av_interleaved_write_frame(m_fmtCtx, pkt);
        av_packet_unref(pkt);
    }
    avcodec_send_frame(m_aCtx, nullptr);
    while(avcodec_receive_packet(m_aCtx, pkt) >= 0) {
        av_packet_rescale_ts(pkt, m_aCtx->time_base, m_aStream->time_base);
        pkt->stream_index = m_aStream->index;
        av_interleaved_write_frame(m_fmtCtx, pkt);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
}

// 【关键修复】确保指针置空，防止二次录制时的状态污染
void FFmpegRecorder::closeResources() {
    if (m_vFrame) { av_frame_free(&m_vFrame); m_vFrame = nullptr; }
    if (m_aFrame) { av_frame_free(&m_aFrame); m_aFrame = nullptr; }
    if (m_vCtx) { avcodec_free_context(&m_vCtx); m_vCtx = nullptr; }
    if (m_aCtx) { avcodec_free_context(&m_aCtx); m_aCtx = nullptr; }
    if (m_fmtCtx) {
        if (!(m_fmtCtx->oformat->flags & AVFMT_NOFILE)) avio_closep(&m_fmtCtx->pb);
        avformat_free_context(m_fmtCtx);
        m_fmtCtx = nullptr;
    }
    if (m_swsCtx) { sws_freeContext(m_swsCtx); m_swsCtx = nullptr; }

    for(auto &k : m_audioSources.keys()) {
        if (m_audioSources[k].swrCtx) swr_free(&m_audioSources[k].swrCtx);
        if (m_audioSources[k].fifo) av_audio_fifo_free(m_audioSources[k].fifo);
    }
    m_audioSources.clear();
}
