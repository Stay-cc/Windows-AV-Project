#include "VideoSourceWorker.h"
#include <QDebug>

VideoSourceWorker::VideoSourceWorker(QVideoSink* sink, int itemId, QObject *parent)
    : QObject(parent), m_sink(sink), m_itemId(itemId)
{
    // 预分配一些内存（可选，Qt的隐式共享通常会自动处理）
}

VideoSourceWorker::~VideoSourceWorker() {
}

void VideoSourceWorker::processFrame() {
    if (!m_sink) return;

    QVideoFrame frame = m_sink->videoFrame();
    if (!frame.isValid()) return;

    // 【核心优化】
    // videoFrame() 返回的数据通常在 GPU 或受保护内存中。
    // map() 操作是将数据从 GPU/驱动层 搬运到 CPU 内存的关键步骤。
    // 在这个独立线程做 map，不会阻塞 UI 线程。

    if (frame.map(QVideoFrame::ReadOnly)) {
        // 使用双缓冲索引
        QImage::Format targetFmt = QImage::Format_ARGB32_Premultiplied;

        // 只有当尺寸变了或者格式不对时才重新转换
        // 注意：QVideoFrame::toImage() 内部通常会做一次深拷贝
        QImage wrapper = frame.toImage();

        // 如果格式不符合 D3D11 预期，进行转换
        if (wrapper.format() != targetFmt) {
            m_buffer[m_writeIndex] = wrapper.convertToFormat(targetFmt);
        } else {
            m_buffer[m_writeIndex] = wrapper.copy(); // 深拷贝以脱离 frame 的生命周期
        }

        frame.unmap();

        // 发送信号
        // 注意：这里发送的是 QImage 的副本（浅拷贝引用计数），开销很小
        emit frameReadyForCompositor(m_itemId, m_buffer[m_writeIndex]);

        // 为了 UI 性能，可以考虑在这里做一个缩放（Scale），但这取决于 UI 需求
        // 这里直接发原图给 UI（UI线程 setPixmap 会处理）
        emit frameReadyForUI(m_buffer[m_writeIndex]);

        // 切换缓冲索引
        m_writeIndex = 1 - m_writeIndex;
    }
}
