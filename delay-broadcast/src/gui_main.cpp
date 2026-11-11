#include "main_window.h"

#include <QApplication>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("delay-broadcast-sim");

    dbsgui::MainWindow window;
    window.show();
    return app.exec();
}
