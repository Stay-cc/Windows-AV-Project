#include "FFmpegRecorder.h"
#include <QDebug>
#include <QDateTime>
#include <QCoreApplication>
#include <algorithm> // for std::clamp

static void printError(const QString& tag, int errCode) {
    char errBuf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errCode, errBuf, AV_ERROR_MAX_STRING_SIZE);
    qCritical() << "[FFmpegError]" << tag << ":" << errBuf << "(Code:" << errCode << ")";
}

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
    qDebug() << "[Recorder]" << msg;
}

bool FFmpegRecorder::startRecord(const QString &filename, int width, int height) {
    if (isRunning()) return false;
    m_filename = filename;
    m_width = width - (width % 2);
    m_height = height - (height % 2);

    { QMutexLocker l(&m_videoMutex); m_videoQueue.clear(); }
    { QMutexLocker l(&m_audioMutex); m_audioQueue.clear(); }

    m_stopRequested = false;
    m_firstFrameTimestamp = -1;
    // 重置淡入标记
    m_hasAppliedFadeIn = false;

    start();
    return true;
}

void FFmpegRecorder::stopRecord() {
    if (isRunning()) {
        m_stopRequested = true;
    }
}

static std::atomic<int> s_queuedFrames{0};
static std::atomic<int> s_encodedFrames{0};
static std::atomic<int> s_writtenFrames{0};
static std::atomic<int> s_audioBytesRcv{0};

void FFmpegRecorder::pushVideoFrame(const QImage &img, qint64 timestamp) {
    if (!m_isRecording || m_stopRequested) return;
    QMutexLocker locker(&m_videoMutex);
    if (m_videoQueue.size() > 100) {
        static int dropCount = 0;
        if (++dropCount % 60 == 0) qWarning() << "Queue Full! Drops:" << dropCount;
        locker.unlock();
        return;
    }
    VideoFrameData data;
    if (img.format() != QImage::Format_RGBA8888 && img.format() != QImage::Format_RGB32) {
        data.image = img.convertToFormat(QImage::Format_RGBA8888);
    } else {
        data.image = img.copy();
    }
    data.timestamp = timestamp;
    m_videoQueue.enqueue(data);
    s_queuedFrames++;
}

void FFmpegRecorder::pushAudioData(int sourceId, const QByteArray &data, int sampleRate, int channels) {
    if (!m_isRecording || m_stopRequested) return;

    // 1. 启动对齐：视频未开始前，丢弃音频
    if (m_firstFrameTimestamp == -1) return;

    QByteArray processedData = data;

    // 2. 淡入处理 (消除开头爆音)
    if (!m_hasAppliedFadeIn) {
        float* samples = reinterpret_cast<float*>(processedData.data());
        int totalSamples = processedData.size() / sizeof(float);
        int fadeLen = 4096;
        if (fadeLen > totalSamples) fadeLen = totalSamples;
        for (int i = 0; i < fadeLen; ++i) {
            float factor = (float)i / (float)fadeLen;
            samples[i] *= factor;
        }
        m_hasAppliedFadeIn = true;
    }

    s_audioBytesRcv += processedData.size();

    QMutexLocker locker(&m_audioMutex);
    AudioChunk chunk;
    chunk.data = processedData;
    chunk.sampleRate = sampleRate;
    chunk.channels = channels;
    chunk.timestamp = 0;
    m_audioQueue.enqueue(qMakePair(sourceId, chunk));
}

void FFmpegRecorder::ensureAudioSource(int id) {
    if (!m_audioSources.contains(id)) {
        SourceContext ctx;
        ctx.fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, 2, 1);
        m_audioSources.insert(id, ctx);
    }
}

// 核心修改：基于视频时间轴驱动音频编码
void FFmpegRecorder::run() {
    qDebug() << "Recorder Thread Started.";

    if (!initOutput(m_filename)) { qCritical() << "Init Output Failed!"; return; }
    if (!initVideoEncoder(m_width, m_height)) { qCritical() << "Init Video Failed!"; closeResources(); return; }
    if (!initAudioEncoder()) { qCritical() << "Init Audio Failed!"; closeResources(); return; }

    ensureAudioSource(0); // Sys
    ensureAudioSource(1); // Mic (提前占位，防止后面找不到)

    if (avformat_write_header(m_fmtCtx, nullptr) < 0) {
        qCritical() << "Write header failed";
        closeResources();
        return;
    }

    m_isRecording = true;
    m_totalAudioSamples = 0;
    s_queuedFrames = 0; s_encodedFrames = 0; s_writtenFrames = 0; s_audioBytesRcv = 0;

    QElapsedTimer logTimer; logTimer.start();
    qDebug() << "Recording Initialization SUCCESS. Waiting for first video frame...";

    qint64 lastVideoPts = 0;

    // 缓存区，避免频繁 new/delete
    int frameSize = m_aCtx->frame_size;
    float *mixBufferL = new float[frameSize];
    float *mixBufferR = new float[frameSize];
    float *tmpL = new float[frameSize];
    float *tmpR = new float[frameSize];
    void *tmpData[2] = { tmpL, tmpR };

    while (!m_stopRequested) {
        QCoreApplication::processEvents();

        // --- 日志 ---
        if (logTimer.elapsed() >= 1000) {
            double audioSec = (double)m_totalAudioSamples / m_aCtx->sample_rate;
            double videoSec = (double)lastVideoPts / 1000.0;
            qDebug() << QString("[STATS] Video: %1s | Audio: %2s | A-Sync: %3ms")
                            .arg(videoSec, 0, 'f', 2)
                            .arg(audioSec, 0, 'f', 2)
                            .arg((audioSec - videoSec)*1000, 0, 'f', 0);
            logTimer.restart();
        }

        // --- 1. 处理视频 ---
        VideoFrameData vFrameData;
        bool hasVideo = false;
        {
            QMutexLocker locker(&m_videoMutex);
            if (!m_videoQueue.isEmpty()) {
                vFrameData = m_videoQueue.dequeue();
                hasVideo = true;
            }
        }
        if (hasVideo) {
            emit frameProcessed();
            processVideo(vFrameData);
            lastVideoPts = vFrameData.timestamp;
        }

        // --- 2. 搬运音频数据到 FIFO ---
        while(true) {
            QPair<int, AudioChunk> chunk;
            bool hasAudio = false;
            {
                QMutexLocker locker(&m_audioMutex);
                if (m_audioQueue.isEmpty()) break;
                chunk = m_audioQueue.dequeue();
                hasAudio = true;
            }
            if (hasAudio) {
                resampleToFifo(chunk.first, chunk.second.data, chunk.second.sampleRate, chunk.second.channels);
            } else {
                break;
            }
        }

        // --- 3. 核心混音逻辑 (Video-Driven) ---
        // 只有当已经开始录制视频(m_firstFrameTimestamp != -1) 且 音频落后于视频时，才生成音频
        // 这保证了音频绝对不会超过视频长度，也不会落后
        if (m_firstFrameTimestamp != -1) {

            // 计算当前视频时间对应的目标音频样本数
            qint64 targetSamples = av_rescale_q(lastVideoPts, {1, 1000}, {1, m_aCtx->sample_rate});

            // 如果当前已编码样本数 < 目标样本数，说明音频不够，需要生成 (混音或补静音)
            while (m_totalAudioSamples < targetSamples) {

                // 清空混音缓冲区 (相当于底噪为静音)
                memset(mixBufferL, 0, frameSize * sizeof(float));
                memset(mixBufferR, 0, frameSize * sizeof(float));

                // 遍历所有音频源 (System 和 Mic)
                for (auto key : m_audioSources.keys()) {
                    SourceContext &ctx = m_audioSources[key];
                    if (!ctx.fifo) continue;

                    // 如果该源有足够的数据 -> 读取并混合
                    if (av_audio_fifo_size(ctx.fifo) >= frameSize) {
                        av_audio_fifo_read(ctx.fifo, tmpData, frameSize);

                        // 混合算法：带钳制 (Clamping) 的加法
                        // 解决 "时大时小" 和 "杂音" 问题
                        for (int i = 0; i < frameSize; ++i) {
                            mixBufferL[i] += tmpL[i];
                            mixBufferR[i] += tmpR[i];
                        }
                    }
                    // 如果该源数据不足 -> 什么都不做 (相当于混入 0.0 静音)
                    // 这样即使麦克风卡顿，系统声音也能正常录制，反之亦然
                }

                // --- 最终输出前的硬限幅 (Hard Limiting) ---
                // 防止多路叠加导致数值超过 1.0 产生爆音
                for (int i = 0; i < frameSize; ++i) {
                    if (mixBufferL[i] > 1.0f) mixBufferL[i] = 1.0f;
                    if (mixBufferL[i] < -1.0f) mixBufferL[i] = -1.0f;

                    if (mixBufferR[i] > 1.0f) mixBufferR[i] = 1.0f;
                    if (mixBufferR[i] < -1.0f) mixBufferR[i] = -1.0f;
                }

                // 编码这一帧
                av_frame_make_writable(m_aFrame);
                memcpy(m_aFrame->data[0], mixBufferL, frameSize * sizeof(float));
                memcpy(m_aFrame->data[1], mixBufferR, frameSize * sizeof(float));

                m_aFrame->pts = av_rescale_q(m_totalAudioSamples, {1, m_aCtx->sample_rate}, m_aCtx->time_base);
                m_totalAudioSamples += frameSize;

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
        }

        if (!hasVideo && !m_stopRequested) QThread::msleep(1);
    }

    // 清理缓冲区
    delete[] mixBufferL; delete[] mixBufferR;
    delete[] tmpL; delete[] tmpR;

    qDebug() << "Stopping... Final Video PTS Cutoff:" << lastVideoPts;

    // --- 结束处理 ---
    // 不再处理剩余的音频 FIFO，直接丢弃！
    // 这样就解决了 "视频10秒，音频13秒" 的问题。
    // 我们只 flush 视频编码器，确保视频完整即可。

    // Flush Video Encoder
    avcodec_send_frame(m_vCtx, nullptr);
    AVPacket *pkt = av_packet_alloc();
    while(avcodec_receive_packet(m_vCtx, pkt) >= 0) {
        av_packet_rescale_ts(pkt, m_vCtx->time_base, m_vStream->time_base);
        pkt->stream_index = m_vStream->index;
        av_interleaved_write_frame(m_fmtCtx, pkt);
        av_packet_unref(pkt);
    }

    // Flush Audio Encoder (仅清空编码器内部缓存，不读新数据)
    avcodec_send_frame(m_aCtx, nullptr);
    while(avcodec_receive_packet(m_aCtx, pkt) >= 0) {
        av_packet_rescale_ts(pkt, m_aCtx->time_base, m_aStream->time_base);
        pkt->stream_index = m_aStream->index;
        av_interleaved_write_frame(m_fmtCtx, pkt);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    av_write_trailer(m_fmtCtx);
    closeResources();
    m_isRecording = false;
    qDebug() << "Recording Finished.";
}

// 辅助函数保持不变
bool FFmpegRecorder::hasAudioPending() {
    QMutexLocker l(&m_audioMutex);
    return !m_audioQueue.isEmpty();
}

void FFmpegRecorder::flushAudioFifo() {
    // 已经不需要这个函数了，因为我们采取了"截断"策略
}

bool FFmpegRecorder::initOutput(const QString &filename) {
    int ret = avformat_alloc_output_context2(&m_fmtCtx, nullptr, nullptr, filename.toStdString().c_str());
    if (!m_fmtCtx || ret < 0) return false;
    if (!(m_fmtCtx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&m_fmtCtx->pb, filename.toStdString().c_str(), AVIO_FLAG_WRITE) < 0) return false;
    }
    return true;
}

bool FFmpegRecorder::initVideoEncoder(int width, int height) {
    const char* encoderNames[] = { "h264_nvenc", "h264_qsv", "libx264", nullptr };
    const AVCodec *codec = nullptr;
    for (const char** name = encoderNames; *name != nullptr; name++) {
        codec = avcodec_find_encoder_by_name(*name);
        if (codec) { qDebug() << "Selected Video Encoder:" << *name; break; }
    }
    if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) return false;

    m_vStream = avformat_new_stream(m_fmtCtx, codec);
    m_vCtx = avcodec_alloc_context3(codec);
    m_vCtx->width = width; m_vCtx->height = height;
    m_vCtx->time_base = {1, 1000000};
    m_vCtx->framerate = {30, 1};
    m_vCtx->pix_fmt = QString(codec->name).contains("nvenc") ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
    m_vCtx->gop_size = 30; m_vCtx->max_b_frames = 0; m_vCtx->bit_rate = 8000000;

    if (QString(codec->name) == "libx264") {
        av_opt_set(m_vCtx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(m_vCtx->priv_data, "tune", "zerolatency", 0);
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
    if (!codec) { qCritical() << "AAC Not Found"; return false; }

    m_aStream = avformat_new_stream(m_fmtCtx, codec);
    m_aCtx = avcodec_alloc_context3(codec);
    m_aCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    m_aCtx->bit_rate = 128000; m_aCtx->sample_rate = 44100;
    AVChannelLayout layout; av_channel_layout_default(&layout, 2);
    av_channel_layout_copy(&m_aCtx->ch_layout, &layout);
    av_channel_layout_uninit(&layout);
    m_aCtx->time_base = {1, m_aCtx->sample_rate};
    if (m_fmtCtx->oformat->flags & AVFMT_GLOBALHEADER) m_aCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(m_aCtx, codec, nullptr) < 0) { printError("Audio Open", -1); return false; }

    avcodec_parameters_from_context(m_aStream->codecpar, m_aCtx);
    m_aFrame = av_frame_alloc();
    m_aFrame->nb_samples = m_aCtx->frame_size;
    m_aFrame->format = m_aCtx->sample_fmt;
    av_channel_layout_copy(&m_aFrame->ch_layout, &m_aCtx->ch_layout);
    m_aFrame->sample_rate = m_aCtx->sample_rate;
    av_frame_get_buffer(m_aFrame, 0);
    return true;
}

void FFmpegRecorder::processVideo(const VideoFrameData &frameData) {
    if (m_firstFrameTimestamp == -1) m_firstFrameTimestamp = frameData.timestamp;
    if (frameData.image.isNull()) return;

    if (!m_swsCtx) {
        AVPixelFormat inputFmt = (frameData.image.format() == QImage::Format_RGBA8888) ? AV_PIX_FMT_RGBA : AV_PIX_FMT_BGRA;
        m_swsCtx = sws_getContext(frameData.image.width(), frameData.image.height(), inputFmt,
                                  m_vCtx->width, m_vCtx->height, m_vCtx->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
    }

    const uint8_t *srcSlice[] = { frameData.image.constBits() };
    int srcStride[] = { (int)frameData.image.bytesPerLine() };
    sws_scale(m_swsCtx, srcSlice, srcStride, 0, frameData.image.height(), m_vFrame->data, m_vFrame->linesize);

    qint64 diffMs = frameData.timestamp;
    m_vFrame->pts = av_rescale_q(diffMs * 1000, {1, 1000000}, m_vCtx->time_base);

    if (avcodec_send_frame(m_vCtx, m_vFrame) >= 0) {
        s_encodedFrames++;
        AVPacket *pkt = av_packet_alloc();
        while (avcodec_receive_packet(m_vCtx, pkt) >= 0) {
            av_packet_rescale_ts(pkt, m_vCtx->time_base, m_vStream->time_base);
            pkt->stream_index = m_vStream->index;
            av_interleaved_write_frame(m_fmtCtx, pkt);
            s_writtenFrames++;
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
    }
}

bool FFmpegRecorder::resampleToFifo(int sourceId, const QByteArray &data, int sampleRate, int channels) {
    ensureAudioSource(sourceId);
    SourceContext &ctx = m_audioSources[sourceId];
    if (data.isEmpty()) return true;

    if (!ctx.swrCtx || ctx.inSampleRate != sampleRate) {
        if (ctx.swrCtx) swr_free(&ctx.swrCtx);
        AVChannelLayout outLayout; av_channel_layout_copy(&outLayout, &m_aCtx->ch_layout);
        AVChannelLayout inLayout; av_channel_layout_default(&inLayout, channels);
        swr_alloc_set_opts2(&ctx.swrCtx, &outLayout, AV_SAMPLE_FMT_FLTP, m_aCtx->sample_rate,
                            &inLayout, AV_SAMPLE_FMT_FLT, sampleRate, 0, nullptr);
        swr_init(ctx.swrCtx);
        ctx.inSampleRate = sampleRate;
        av_channel_layout_uninit(&inLayout); av_channel_layout_uninit(&outLayout);
    }

    int inputSamples = data.size() / (channels * sizeof(float));
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

// 注意：旧的 processAudio 函数已经移除，逻辑完全整合到了 run() 循环中
void FFmpegRecorder::processAudio() {}

void FFmpegRecorder::flushEncoders() {
    // 逻辑已整合到 run() 尾部
}

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
