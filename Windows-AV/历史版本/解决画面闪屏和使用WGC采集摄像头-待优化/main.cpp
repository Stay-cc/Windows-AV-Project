#include "MainWindow.h"
#include "SystemController.h"
#include <QApplication>

int main(int argc, char *argv[])
{
    // 适配高分屏
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);

    QApplication a(argc, argv);

    // 启动后台逻辑控制器 (单例模式或独立实例)
    SystemController controller;
    controller.initialize();

    // 启动无边框 UI
    MainWindow w;
    w.show();

    return a.exec();
}
