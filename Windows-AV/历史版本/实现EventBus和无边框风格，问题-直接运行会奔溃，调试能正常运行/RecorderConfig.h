#ifndef RECORDERCONFIG_H
#define RECORDERCONFIG_H

#include <QString>

struct RecorderConfig {
    // --- 视频参数 ---
    int width = 1920;
    int height = 1080;
    int fps = 30;
    int videoBitrate = 6000000; // 6 Mbps
    QString encoderName = "h264_nvenc";
    QString savePath = "";

    // --- 音频参数 ---
    int audioSampleRate = 44100;
    int audioBitrate = 128000;
    int audioChannels = 2;
    int fadeInDurationMs = 300;
    int fadeOutDurationMs = 500;

    // --- [新增] 窗口行为参数 ---
    bool windowResizable = false;    // 默认锁定
    bool windowMaximizable = false;  // 默认禁用最大化
};

#endif // RECORDERCONFIG_H
