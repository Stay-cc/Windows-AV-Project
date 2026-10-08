#ifndef VIDEOSOURCEWORKER_H
#define VIDEOSOURCEWORKER_H

#include <QObject>
#include <QVideoSink>
#include <QVideoFrame>
#include <QImage>
#include <QMutex>

// 专门处理视频采集、转换的工作者，运行在独立线程
class VideoSourceWorker : public QObject
{
    Q_OBJECT
public:
    explicit VideoSourceWorker(QVideoSink* sink, int itemId, QObject *parent = nullptr);
    ~VideoSourceWorker();

public slots:
    void processFrame(); // 响应视频帧到达

signals:
    // 通知 UI 更新预览
    void frameReadyForUI(QImage img);
    // 通知合成器更新 GPU 纹理
    void frameReadyForCompositor(int itemId, QImage img);

private:
    QVideoSink* m_sink;
    int m_itemId;

    // 双缓冲机制，避免反复 new/delete
    QImage m_buffer[2];
    int m_writeIndex = 0;
};

#endif // VIDEOSOURCEWORKER_H
