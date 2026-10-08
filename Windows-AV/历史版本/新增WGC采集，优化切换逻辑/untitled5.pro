QT       += core gui

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets multimedia multimediawidgets

CONFIG += c++20

# 开启 MSVC 标准兼容模式 (对 WGC 至关重要)
QMAKE_CXXFLAGS += /permissive- /Zc:twoPhase-

SOURCES += \
    DxGiCapturer.cpp \
    ResizablePixmapItem.cpp \
    VideoSourceWorker.cpp \
    WgcCapturer.cpp \
    main.cpp \
    mainwindow.cpp

HEADERS += \
    DxGiCapturer.h \
    ResizablePixmapItem.h \
    VideoSourceWorker.h \
    WgcCapturer.h \
    mainwindow.h

FORMS += \
    mainwindow.ui

# 【关键】链接 User32.lib 以支持窗口枚举 API
LIBS += -ld3d11 -ldxgi -ld3dcompiler -ldwmapi -lwindowsapp -luser32

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
