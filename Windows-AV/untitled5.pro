QT       += core gui

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets multimedia multimediawidgets concurrent

CONFIG += c++20

# 开启 MSVC 标准兼容模式 (对 WGC 至关重要)
QMAKE_CXXFLAGS += /permissive- /Zc:twoPhase-

SOURCES += \
    DxGiCapturer.cpp \
    FFmpegRecorder.cpp \
    FramelessWidget.cpp \
    ResizablePixmapItem.cpp \
    SettingsDialog.cpp \
    SystemController.cpp \
    WasapiCapturer.cpp \
    WgcCapturer.cpp \
    WinRTCamera.cpp \
    form.cpp \
    main.cpp \
    mainwindow.cpp

HEADERS += \
    AddSourceDialog.h \
    CrashHandler.h \
    DxGiCapturer.h \
    EventBus.h \
    FFmpegRecorder.h \
    FramelessWidget.h \
    Logger.h \
    RecorderConfig.h \
    ResizablePixmapItem.h \
    ResizableTextItem.h \
    SettingsDialog.h \
    SystemController.h \
    WasapiCapturer.h \
    WgcCapturer.h \
    WinRTCamera.h \
    form.h \
    mainwindow.h



LIBS += -L$(FFMPEG_PATH)/lib -lavcodec -lavdevice -lavformat -lavutil -lswresample -lswscale


# 【关键】链接 User32.lib 以支持窗口枚举 API
LIBS += -ld3d11 -ldxgi -ld3dcompiler -ldwmapi -lwindowsapp -luser32 -lmmdevapi -lole32

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

# FFmpeg 配置
win32 {
    FFMPEG_PATH = D:/DownLoad/ffmpeg
    INCLUDEPATH += $$FFMPEG_PATH/include
    LIBS += -L$$FFMPEG_PATH/lib \
        -lavcodec \
        -lavformat \
        -lavutil \
        -lswscale \
        -lswresample
}

FORMS += \
    form.ui
