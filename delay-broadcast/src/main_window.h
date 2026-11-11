#pragma once

#include "engine_controller.h"
#include "timeline_widget.h"

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>

namespace dbsgui {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

    // Read-only live engine positions (used by tests and the timeline).
    EngineView controllerView() { return controller_.view(); }

private slots:
    void startEngine();
    void stopEngine();
    void submitEdit();
    void undoLast();
    void refreshView();
    void appendLog(const QString& line);
    void onControlResolved(quint64 id, const QString& status, const QString& detail);

private:
    EngineController controller_;
    TimelineWidget* timeline_;
    QTimer uiTimer_;

    QSpinBox* delayMs_;
    QSpinBox* capacityMs_;
    QSpinBox* fadeMs_;
    QComboBox* gapMode_;
    QComboBox* eoiMode_;
    QLineEdit* beginSec_;
    QLineEdit* endSec_;
    QComboBox* padBox_;
    QPushButton* startBtn_;
    QPushButton* stopBtn_;
    QPushButton* editBtn_;
    QPushButton* undoBtn_;
    QPlainTextEdit* logView_;
    QLabel* stateLabel_;

    quint64 lastAcceptedEdit_ = 0;
};

}  // namespace dbsgui
