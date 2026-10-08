#include "FFmpegRecorder.h"
#include <QDebug>
#include <QDateTime>
#include <QCoreApplication>

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

    // [新增] 重置淡入标记
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

    // 1. 启动对齐：如果视频还没开始，丢弃音频
    if (m_firstFrameTimestamp == -1) {
        return;
    }

    // [新增] 2. 开头音频淡入处理 (Fade-In)
    // 目的：消除从静音到有声的瞬间跳变导致的"爆破音/杂音"
    // 只对第一个进入的音频包（通常是系统声音）做处理
    QByteArray processedData = data; // 拷贝一份数据进行修改

    if (!m_hasAppliedFadeIn) {
        float* samples = reinterpret_cast<float*>(processedData.data());
        int totalSamples = processedData.size() / sizeof(float);

        // 设定淡入长度：例如 4096 个采样点 (约 90ms @ 44.1kHz)
        // 也就是前 90ms 音量从 0 线性增加到 1
        int fadeLen = 4096;
        if (fadeLen > totalSamples) fadeLen = totalSamples;

        for (int i = 0; i < fadeLen; ++i) {
            float factor = (float)i / (float)fadeLen; // 0.0 -> 1.0
            samples[i] *= factor;
        }

        // 标记已完成，后续数据不再处理
        m_hasAppliedFadeIn = true;
        // qDebug() << "[Audio] Fade-in applied to start samples.";
    }

    s_audioBytesRcv += processedData.size();

    QMutexLocker locker(&m_audioMutex);
    AudioChunk chunk;
    chunk.data = processedData; // 使用处理后的数据
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

void FFmpegRecorder::run() {
    qDebug() << "Recorder Thread Started.";

    if (!initOutput(m_filename)) { qCritical() << "Init Output Failed!"; return; }
    if (!initVideoEncoder(m_width, m_height)) { qCritical() << "Init Video Failed!"; closeResources(); return; }
    if (!initAudioEncoder()) { qCritical() << "Init Audio Failed!"; closeResources(); return; }

    ensureAudioSource(0);

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

    while (!m_stopRequested) {
        QCoreApplication::processEvents();

        if (logTimer.elapsed() >= 1000) {
            // 计算音频延迟/超前情况
            double audioSec = (double)m_totalAudioSamples / m_aCtx->sample_rate;
            double videoSec = (double)lastVideoPts / 1000.0;
            qDebug() << QString("[STATS] Video: %1s | Audio: %2s | Diff: %3ms")
                            .arg(videoSec, 0, 'f', 2)
                            .arg(audioSec, 0, 'f', 2)
                            .arg((videoSec - audioSec)*1000, 0, 'f', 0);
            s_audioBytesRcv = 0;
            logTimer.restart();
        }

        // --- 视频处理 ---
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
            emit frameProcessed(); // 通知采集器继续
            processVideo(vFrameData);
            lastVideoPts = vFrameData.timestamp; // 更新当前视频时间
        }

        // --- 音频队列读取与重采样 ---
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

        // --- [优化2] 更激进的音画同步补帧逻辑 ---
        if (m_firstFrameTimestamp != -1 && m_aCtx && m_audioSources.contains(0)) {
            qint64 expectedSamples = av_rescale_q(lastVideoPts, {1, 1000}, {1, m_aCtx->sample_rate});

            qint64 currentFifoSize = 0;
            if (m_audioSources[0].fifo) {
                currentFifoSize = av_audio_fifo_size(m_audioSources[0].fifo);
            }
            qint64 actualSamples = m_totalAudioSamples + currentFifoSize;
            qint64 lagSamples = expectedSamples - actualSamples;
            double lagMs = (double)lagSamples * 1000.0 / m_aCtx->sample_rate;

            // 阈值：40ms
            if (lagMs > 40.0) {
                SourceContext &ctx = m_audioSources[0];
                if (ctx.fifo) {
                    int padCount = (int)lagSamples;
                    int maxPad = m_aCtx->sample_rate / 10;
                    if (padCount > maxPad) padCount = maxPad;

                    if (padCount > 0) {
                        float* silence = (float*)calloc(padCount, sizeof(float));
                        void* data[2] = { silence, silence };

                        if (av_audio_fifo_space(ctx.fifo) < padCount) {
                            av_audio_fifo_realloc(ctx.fifo, av_audio_fifo_size(ctx.fifo) + padCount);
                        }

                        av_audio_fifo_write(ctx.fifo, data, padCount);
                        free(silence);
                    }
                }
            }
        }

        // --- 音频混合与编码 ---
        processAudio();

        if (!hasVideo && !hasAudioPending()) QThread::msleep(1);
    }

    qDebug() << "Stopping... Final Video PTS Cutoff:" << lastVideoPts;
    qint64 stopCutoffTime = lastVideoPts + 200;

    // Flush Video
    while (true) {
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
            if (vFrameData.timestamp > stopCutoffTime) continue;
            processVideo(vFrameData);
        } else {
            break;
        }
    }

    // Flush Audio
    flushAudioFifo();

    flushEncoders();
    av_write_trailer(m_fmtCtx);
    closeResources();
    m_isRecording = false;
    qDebug() << "Recording Finished.";
}

// 辅助函数：检查是否有待处理的音频数据
bool FFmpegRecorder::hasAudioPending() {
    // 检查 Audio Queue
    {
        QMutexLocker l(&m_audioMutex);
        if (!m_audioQueue.isEmpty()) return true;
    }
    // 检查 FIFO
    for(auto &ctx : m_audioSources) {
        if (ctx.fifo && av_audio_fifo_size(ctx.fifo) >= m_aCtx->frame_size) return true;
    }
    return false;
}

// 辅助函数：结束时清空 FIFO
void FFmpegRecorder::flushAudioFifo() {
    int frameSize = m_aCtx->frame_size;
    // 简单的补齐逻辑，确保残余数据被写出
    for (auto key : m_audioSources.keys()) {
        if (!m_audioSources[key].fifo) continue;
        int currentSize = av_audio_fifo_size(m_audioSources[key].fifo);
        if (currentSize > 0 && currentSize < frameSize) {
            int padding = frameSize - currentSize;
            float* silence = (float*)calloc(padding, sizeof(float));
            void* data[2] = { silence, silence };
            if (av_audio_fifo_space(m_audioSources[key].fifo) < padding) {
                av_audio_fifo_realloc(m_audioSources[key].fifo, av_audio_fifo_size(m_audioSources[key].fifo) + padding);
            }
            av_audio_fifo_write(m_audioSources[key].fifo, data, padding);
            free(silence);
        }
    }
    processAudio();
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

void FFmpegRecorder::processAudio() {
    int frameSize = m_aCtx->frame_size;
    int maxAvail = 0;
    for(auto &ctx : m_audioSources) maxAvail = qMax(maxAvail, av_audio_fifo_size(ctx.fifo));

    while (maxAvail >= frameSize) {
        av_frame_make_writable(m_aFrame);
        float *dstL = (float*)m_aFrame->data[0];
        float *dstR = (float*)m_aFrame->data[1];

        memset(dstL, 0, frameSize * sizeof(float));
        memset(dstR, 0, frameSize * sizeof(float));

        float *tmpL = new float[frameSize];
        float *tmpR = new float[frameSize];
        void *tmpData[2] = { tmpL, tmpR };

        for(auto &ctx : m_audioSources) {
            if (av_audio_fifo_size(ctx.fifo) >= frameSize) {
                av_audio_fifo_read(ctx.fifo, tmpData, frameSize);
                for(int i=0; i<frameSize; ++i) {
                    dstL[i] += tmpL[i];
                    dstR[i] += tmpR[i];
                }
            }
        }
        delete[] tmpL; delete[] tmpR;

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

        maxAvail = 0;
        for(auto &ctx : m_audioSources) maxAvail = qMax(maxAvail, av_audio_fifo_size(ctx.fifo));
    }
}

void FFmpegRecorder::flushEncoders() {
    if (!m_vCtx || !m_aCtx) return;
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
