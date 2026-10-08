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
    m_softStopRequested = false;
    this->moveToThread(this);
}

FFmpegRecorder::~FFmpegRecorder() {
    if (isRunning()) {
        m_softStopRequested = true;
        wait();
    }
}

// [修改] 接收配置对象
bool FFmpegRecorder::startRecord(const RecorderConfig& config) {
    if (isRunning()) return false;
    m_config = config;

    // 1. 如果路径为空，自动生成
    if (m_config.savePath.isEmpty()) {
        m_config.savePath = "REC_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".mp4";
    }

    // 2. 视频宽高必须是偶数
    m_config.width -= (m_config.width % 2);
    m_config.height -= (m_config.height % 2);

    { QMutexLocker l(&m_videoMutex); m_videoQueue.clear(); }
    { QMutexLocker l(&m_audioMutex); m_audioQueue.clear(); }

    m_softStopRequested = false;
    m_firstFrameTimestamp = -1;
    start();
    return true;
}

void FFmpegRecorder::stopRecord() {
    if (isRunning()) {
        // [关键] 触发软停止，进入淡出流程
        m_softStopRequested = true;
    }
}

void FFmpegRecorder::pushVideoFrame(const QImage &img, qint64 timestamp) {
    if (!m_isRecording || m_softStopRequested) return; // 停止请求后不再接收新视频
    QMutexLocker locker(&m_videoMutex);
    if (m_videoQueue.size() > 150) return; // 简单丢帧

    VideoFrameData data;
    // 统一转为 RGBA
    if (img.format() != QImage::Format_RGBA8888) {
        data.image = img.convertToFormat(QImage::Format_RGBA8888);
    } else {
        data.image = img.copy();
    }
    data.timestamp = timestamp;
    m_videoQueue.enqueue(data);
}

void FFmpegRecorder::pushAudioData(int sourceId, const QByteArray &data, int sampleRate, int channels) {
    // 停止请求后也不再接收外部新音频，依赖 FIFO 残留数据做淡出
    if (!m_isRecording || m_softStopRequested) return;
    if (m_firstFrameTimestamp == -1) return; // 严格对齐

    QMutexLocker locker(&m_audioMutex);
    AudioChunk chunk;
    chunk.data = data;
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

// [企业级] 精确的音频淡入淡出计算
void FFmpegRecorder::applyFadeEffect(float* bufferL, float* bufferR, int samples,
                                     qint64 totalEncodedSamples, bool isFadingOut, qint64 fadeOutStartSample)
{
    int sampleRate = m_config.audioSampleRate;
    // 计算淡入需要的总样本数 (比如 300ms = 13230 samples)
    qint64 fadeInSamples = (qint64)(m_config.fadeInDurationMs / 1000.0 * sampleRate);
    // 计算淡出需要的总样本数
    qint64 fadeOutSamples = (qint64)(m_config.fadeOutDurationMs / 1000.0 * sampleRate);

    for (int i = 0; i < samples; ++i) {
        qint64 currentPos = totalEncodedSamples + i;
        float gain = 1.0f;

        // 1. 淡入处理 (Start)
        if (currentPos < fadeInSamples) {
            gain = (float)currentPos / (float)fadeInSamples;
        }

        // 2. 淡出处理 (End)
        if (isFadingOut) {
            qint64 samplesSinceStop = currentPos - fadeOutStartSample;
            if (samplesSinceStop >= 0) {
                if (samplesSinceStop < fadeOutSamples) {
                    // 线性递减: 1.0 -> 0.0
                    gain *= (1.0f - (float)samplesSinceStop / (float)fadeOutSamples);
                } else {
                    gain = 0.0f; // 彻底静音
                }
            }
        }

        bufferL[i] *= gain;
        bufferR[i] *= gain;
    }
}

void FFmpegRecorder::run() {
    qDebug() << "Recorder Thread Started. Config:" << m_config.width << "x" << m_config.height << "FPS:" << m_config.fps;

    if (!initOutput()) { qCritical() << "Init Output Failed"; return; }
    if (!initVideoEncoder()) { qCritical() << "Init Video Failed"; closeResources(); return; }
    if (!initAudioEncoder()) { qCritical() << "Init Audio Failed"; closeResources(); return; }

    ensureAudioSource(0); // Sys
    ensureAudioSource(1); // Mic

    if (avformat_write_header(m_fmtCtx, nullptr) < 0) {
        qCritical() << "Write header failed";
        closeResources();
        return;
    }

    m_isRecording = true;
    m_totalAudioSamples = 0;

    qint64 lastVideoPts = 0;

    // 淡出状态控制
    bool isFadingOut = false;
    qint64 fadeOutStartSample = 0;
    qint64 finalAudioSampleEnd = -1; // 淡出结束后的绝对采样点

    // 预分配缓冲区
    int frameSize = m_aCtx->frame_size;
    float *mixBufferL = new float[frameSize];
    float *mixBufferR = new float[frameSize];
    float *tmpL = new float[frameSize];
    float *tmpR = new float[frameSize];
    void *tmpData[2] = { tmpL, tmpR };

    // 主循环
    while (true) {
        QCoreApplication::processEvents();

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

        // --- 2. 检查停止条件 (软停止 -> 淡出) ---
        if (m_softStopRequested && !isFadingOut) {
            isFadingOut = true;
            fadeOutStartSample = m_totalAudioSamples; // 记录按停止键那一刻的音频位置

            // 计算需要写入多少样本才能完美淡出
            qint64 samplesNeeded = (qint64)(m_config.fadeOutDurationMs / 1000.0 * m_config.audioSampleRate);
            finalAudioSampleEnd = fadeOutStartSample + samplesNeeded;

            qDebug() << "Soft Stop Requested. Fading out from" << fadeOutStartSample << "to" << finalAudioSampleEnd;
        }

        // 如果已经在淡出，并且已经写完了所需的淡出样本，且视频队列也处理完了 -> 退出循环
        if (isFadingOut && m_totalAudioSamples >= finalAudioSampleEnd && !hasVideo && m_videoQueue.isEmpty()) {
            break;
        }

        // --- 3. 搬运音频数据到 FIFO ---
        while(true) {
            QPair<int, AudioChunk> chunk;
            bool hasAudio = false;
            {
                QMutexLocker locker(&m_audioMutex);
                if (!m_audioQueue.isEmpty()) { chunk = m_audioQueue.dequeue(); hasAudio = true; }
            }
            if (hasAudio) {
                resampleToFifo(chunk.first, chunk.second.data, chunk.second.sampleRate, chunk.second.channels);
            } else {
                break;
            }
        }

        // --- 4. 混音与编码 (由视频或淡出逻辑驱动) ---
        if (m_firstFrameTimestamp != -1) {
            qint64 targetSamples;

            if (isFadingOut) {
                // 淡出模式：目标是固定的结束点
                targetSamples = finalAudioSampleEnd;
            } else {
                // 正常模式：目标是视频当前时间
                targetSamples = av_rescale_q(lastVideoPts, {1, 1000}, {1, m_aCtx->sample_rate});
            }

            while (m_totalAudioSamples < targetSamples) {
                // 清零 (底噪)
                memset(mixBufferL, 0, frameSize * sizeof(float));
                memset(mixBufferR, 0, frameSize * sizeof(float));

                // 混音叠加
                for (auto key : m_audioSources.keys()) {
                    SourceContext &ctx = m_audioSources[key];
                    if (!ctx.fifo) continue;
                    if (av_audio_fifo_size(ctx.fifo) >= frameSize) {
                        av_audio_fifo_read(ctx.fifo, tmpData, frameSize);
                        for (int i = 0; i < frameSize; ++i) {
                            mixBufferL[i] += tmpL[i];
                            mixBufferR[i] += tmpR[i];
                        }
                    }
                    // 如果某个源数据不足，相当于加 0 (静音)
                }

                // [核心] 应用淡入淡出增益
                applyFadeEffect(mixBufferL, mixBufferR, frameSize, m_totalAudioSamples, isFadingOut, fadeOutStartSample);

                // [核心] 硬限幅 (Clamping) 防止爆音
                for (int i = 0; i < frameSize; ++i) {
                    mixBufferL[i] = std::clamp(mixBufferL[i], -1.0f, 1.0f);
                    mixBufferR[i] = std::clamp(mixBufferR[i], -1.0f, 1.0f);
                }

                // 写入 Frame
                av_frame_make_writable(m_aFrame);
                memcpy(m_aFrame->data[0], mixBufferL, frameSize * sizeof(float));
                memcpy(m_aFrame->data[1], mixBufferR, frameSize * sizeof(float));

                m_aFrame->pts = av_rescale_q(m_totalAudioSamples, {1, m_aCtx->sample_rate}, m_aCtx->time_base);
                m_totalAudioSamples += frameSize;

                // 发送编码
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

        if (!hasVideo && !m_softStopRequested) QThread::msleep(1);
    }

    delete[] mixBufferL; delete[] mixBufferR; delete[] tmpL; delete[] tmpR;

    flushEncoders();
    av_write_trailer(m_fmtCtx);
    closeResources();
    m_isRecording = false;
    qDebug() << "Recording Finished. Total Audio Samples:" << m_totalAudioSamples;
}

// 辅助函数：初始化输出上下文 (使用 Config)
bool FFmpegRecorder::initOutput() {
    int ret = avformat_alloc_output_context2(&m_fmtCtx, nullptr, nullptr, m_config.savePath.toStdString().c_str());
    if (!m_fmtCtx || ret < 0) return false;
    if (!(m_fmtCtx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&m_fmtCtx->pb, m_config.savePath.toStdString().c_str(), AVIO_FLAG_WRITE) < 0) return false;
    }
    return true;
}

// 辅助函数：初始化视频编码器 (使用 Config)
bool FFmpegRecorder::initVideoEncoder() {
    // 1. 定义一个辅助 Lambda 来尝试打开编码器
    auto tryOpenCodec = [&](QString codecName) -> bool {
        const AVCodec *codec = avcodec_find_encoder_by_name(codecName.toStdString().c_str());
        if (!codec) {
            qWarning() << "Codec not found:" << codecName;
            return false;
        }

        // 如果之前分配过，先清理（防止降级重试时内存泄漏）
        if (m_vCtx) avcodec_free_context(&m_vCtx);
        if (m_vStream) {
            // 注意：avformat_new_stream 分配的流通常由 fmtCtx 管理，但在初始化失败重试时，
            // 我们最好复用流或者清理上下文。这里为了简单，我们假设流一旦创建就复用。
        } else {
            m_vStream = avformat_new_stream(m_fmtCtx, codec);
        }

        m_vCtx = avcodec_alloc_context3(codec);

        // 设置参数
        m_vCtx->width = m_config.width;
        m_vCtx->height = m_config.height;
        m_vCtx->time_base = {1, 1000000}; // 微秒基准
        m_vCtx->framerate = {m_config.fps, 1};
        m_vCtx->gop_size = m_config.fps * 2;
        m_vCtx->max_b_frames = 0;
        m_vCtx->bit_rate = m_config.videoBitrate;

        // 【关键】根据编码器名称自动修正像素格式
        // NVENC/QSV/AMF 通常需要 NV12
        // libx264 推荐 YUV420P
        if (codecName.contains("nvenc") || codecName.contains("qsv") || codecName.contains("amf")) {
            m_vCtx->pix_fmt = AV_PIX_FMT_NV12;
        } else {
            m_vCtx->pix_fmt = AV_PIX_FMT_YUV420P;
        }

        if (m_fmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
            m_vCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        // 针对 CPU 编码的低延迟优化
        if (codecName == "libx264") {
            av_opt_set(m_vCtx->priv_data, "preset", "ultrafast", 0);
            av_opt_set(m_vCtx->priv_data, "tune", "zerolatency", 0);
        }

        // 尝试打开
        int ret = avcodec_open2(m_vCtx, codec, nullptr);
        if (ret < 0) {
            char errBuf[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(ret, errBuf, AV_ERROR_MAX_STRING_SIZE);
            qWarning() << "Failed to open video encoder:" << codecName
                       << "Error:" << errBuf << "(Code:" << ret << ")";
            return false;
        }

        // 成功后，同步参数到流
        avcodec_parameters_from_context(m_vStream->codecpar, m_vCtx);
        qDebug() << "Video Encoder Initialized Successfully:" << codecName;
        return true;
    };

    // 2. 第一次尝试：使用用户配置的编码器 (比如 h264_nvenc)
    if (tryOpenCodec(m_config.encoderName)) {
        goto success;
    }

    // 3. 失败处理：如果用户选的不是 libx264，则尝试降级到 libx264 (CPU)
    if (m_config.encoderName != "libx264") {
        qWarning() << "Hardware encoder failed. Falling back to Software (libx264)...";
        if (tryOpenCodec("libx264")) {
            m_config.encoderName = "libx264"; // 更新配置状态
            goto success;
        }
    }

    // 4. 全部失败
    qCritical() << "All video encoder attempts failed!";
    return false;

success:
    // 分配帧缓存
    if (m_vFrame) av_frame_free(&m_vFrame);
    m_vFrame = av_frame_alloc();
    m_vFrame->width = m_config.width;
    m_vFrame->height = m_config.height;
    m_vFrame->format = m_vCtx->pix_fmt;
    av_frame_get_buffer(m_vFrame, 32);
    return true;
}

// 辅助函数：初始化音频编码器 (使用 Config)
bool FFmpegRecorder::initAudioEncoder() {
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) return false;

    m_aStream = avformat_new_stream(m_fmtCtx, codec);
    m_aCtx = avcodec_alloc_context3(codec);
    m_aCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    m_aCtx->bit_rate = m_config.audioBitrate; // 128k
    m_aCtx->sample_rate = m_config.audioSampleRate; // 44100 or 48000

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

// 视频处理保持不变，只需注意使用 m_config.width/height
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

// resampleToFifo, flushEncoders, closeResources 保持标准实现
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
