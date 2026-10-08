#include "VideoSourceWorker.h"

VideoSourceWorker::VideoSourceWorker(QVideoSink* sink, int itemId, QObject *parent)
    : QObject(parent), m_sink(sink), m_itemId(itemId) {}

VideoSourceWorker::~VideoSourceWorker() {}

void VideoSourceWorker::processFrame() {
    if (!m_sink) return;
    QVideoFrame frame = m_sink->videoFrame();
    if (!frame.isValid()) return;

    if (frame.map(QVideoFrame::ReadOnly)) {
        QImage::Format targetFmt = QImage::Format_ARGB32_Premultiplied;
        QImage wrapper = frame.toImage();
        if (wrapper.format() != targetFmt) {
            m_buffer[m_writeIndex] = wrapper.convertToFormat(targetFmt);
        } else {
            m_buffer[m_writeIndex] = wrapper.copy();
        }
        frame.unmap();
        emit frameReadyForCompositor(m_itemId, m_buffer[m_writeIndex]);
        emit frameReadyForUI(m_buffer[m_writeIndex]);
        m_writeIndex = 1 - m_writeIndex;
    }
}
