#include "main_window.h"

#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTimer>
#include <QMetaObject>

#include <cstdio>

using namespace dbsgui;

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    MainWindow window;
    window.resize(1000, 700);
    window.show();

    // Submit a 3.5-3.7 s replacement, then undo it. Submission is driven by
    // live sample positions, not a guessed wall clock, so it stays reliable
    // under offscreen timer jitter.
    if (auto* b = window.findChild<QLineEdit*>("beginSec")) b->setText("3.5");
    if (auto* e = window.findChild<QLineEdit*>("endSec")) e->setText("3.7");
    if (auto* pad = window.findChild<QComboBox*>("padBox")) pad->setCurrentIndex(0);

    const qint64 kBegin = static_cast<qint64>(3.5 * 48000);
    const qint64 kEnd = static_cast<qint64>(3.7 * 48000);

    enum Phase { Start, WaitSubmit, WaitUndo, Stop, Dump };
    Phase phase = Start;
    QTimer driver;
    driver.setInterval(20);
    QObject::connect(&driver, &QTimer::timeout, [&] {
        switch (phase) {
            case Start:
                QMetaObject::invokeMethod(&window, "startEngine");
                phase = WaitSubmit;
                break;
            case WaitSubmit: {
                // Fire only once the full target is buffered (write head past
                // the end) and still unaired (output head before the begin):
                // exactly the controlled-window contract the UI enforces.
                const dbsgui::EngineView v = window.controllerView();
                if (v.writePos >= kEnd && v.outputPos < kBegin) {
                    QMetaObject::invokeMethod(&window, "submitEdit");
                    phase = WaitUndo;
                }
                break;
            }
            case WaitUndo:
                QMetaObject::invokeMethod(&window, "undoLast");
                phase = Stop;
                break;
            case Stop:
                QMetaObject::invokeMethod(&window, "stopEngine");
                phase = Dump;
                break;
            case Dump: {
                const auto logs = window.findChildren<QPlainTextEdit*>();
                if (!logs.empty()) {
                    std::printf("---- GUI log ----\n%s\n-----------------\n",
                                logs.first()->toPlainText().toUtf8().constData());
                }
                std::printf("GUI smoke complete\n");
                driver.stop();
                app.quit();
                break;
            }
        }
    });
    driver.start();
    QTimer::singleShot(15000, &app, &QApplication::quit);
    return app.exec();
}
