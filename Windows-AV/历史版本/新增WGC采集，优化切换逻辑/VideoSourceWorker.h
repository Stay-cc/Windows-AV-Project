#ifndef VIDEOSOURCEWORKER_H
#define VIDEOSOURCEWORKER_H

#include <QObject>
#include <QVideoSink>
#include <QVideoFrame>
#include <QImage>
#include <QMutex>

class VideoSourceWorker : public QObject
{
    Q_OBJECT
public:
    explicit VideoSourceWorker(QVideoSink* sink, int itemId, QObject *parent = nullptr);
    ~VideoSourceWorker();

public slots:
    void processFrame();

signals:
    void frameReadyForUI(QImage img);
    void frameReadyForCompositor(int itemId, QImage img);

private:
    QVideoSink* m_sink;
    int m_itemId;
    QImage m_buffer[2];
    int m_writeIndex = 0;
};
#endif
