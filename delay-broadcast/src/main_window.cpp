#include "main_window.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QVBoxLayout>

namespace dbsgui {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("延时播出模拟台 — Local Demo Audio Only"));
    resize(1080, 720);

    auto* central = new QWidget;
    auto* root = new QVBoxLayout(central);

    timeline_ = new TimelineWidget;
    root->addWidget(timeline_);

    stateLabel_ = new QLabel(QStringLiteral("stopped"));
    root->addWidget(stateLabel_);

    auto* controls = new QHBoxLayout;

    auto* cfgGroup = new QGroupBox(QStringLiteral("引擎配置（样本驱动）"));
    auto* cfgForm = new QFormLayout(cfgGroup);
    delayMs_ = new QSpinBox;
    delayMs_->setRange(10, 60000);
    delayMs_->setValue(3000);
    capacityMs_ = new QSpinBox;
    capacityMs_->setRange(100, 120000);
    capacityMs_->setValue(8000);
    fadeMs_ = new QSpinBox;
    fadeMs_->setRange(0, 1000);
    fadeMs_->setValue(5);
    gapMode_ = new QComboBox;
    gapMode_->addItems({QStringLiteral("补静音 insert-silence"),
                        QStringLiteral("停播 stop-output")});
    eoiMode_ = new QComboBox;
    eoiMode_->addItems({QStringLiteral("补静音 insert-silence"),
                        QStringLiteral("停播 stop-output")});
    cfgForm->addRow(QStringLiteral("延迟 (ms)"), delayMs_);
    cfgForm->addRow(QStringLiteral("环形容量 (ms)"), capacityMs_);
    cfgForm->addRow(QStringLiteral("接缝淡化 (ms)"), fadeMs_);
    cfgForm->addRow(QStringLiteral("输入中断"), gapMode_);
    cfgForm->addRow(QStringLiteral("输入结束"), eoiMode_);
    controls->addWidget(cfgGroup);

    auto* editGroup = new QGroupBox(QStringLiteral("垫片替换（仅作用于受控区间）"));
    auto* editForm = new QFormLayout(editGroup);
    beginSec_ = new QLineEdit(QStringLiteral("3.0"));
    beginSec_->setObjectName(QStringLiteral("beginSec"));
    endSec_ = new QLineEdit(QStringLiteral("4.0"));
    endSec_->setObjectName(QStringLiteral("endSec"));
    padBox_ = new QComboBox;
    padBox_->setObjectName(QStringLiteral("padBox"));
    for (const auto& n : controller_.padNames()) padBox_->addItem(n);
    editForm->addRow(QStringLiteral("起点 (秒)"), beginSec_);
    editForm->addRow(QStringLiteral("终点 (秒)"), endSec_);
    editForm->addRow(QStringLiteral("垫片"), padBox_);
    editBtn_ = new QPushButton(QStringLiteral("提交替换"));
    undoBtn_ = new QPushButton(QStringLiteral("快速撤销上一条"));
    editForm->addRow(editBtn_, undoBtn_);
    controls->addWidget(editGroup);

    auto* runGroup = new QGroupBox(QStringLiteral("运行"));
    auto* runLayout = new QVBoxLayout(runGroup);
    startBtn_ = new QPushButton(QStringLiteral("启动"));
    stopBtn_ = new QPushButton(QStringLiteral("停止"));
    stopBtn_->setEnabled(false);
    runLayout->addWidget(startBtn_);
    runLayout->addWidget(stopBtn_);
    runLayout->addStretch();
    controls->addWidget(runGroup);

    root->addLayout(controls);

    logView_ = new QPlainTextEdit;
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(2000);
    root->addWidget(logView_, 1);

    auto* notice = new QLabel(
        QStringLiteral("本软件只处理本地演示音流：口令编号音轨由程序生成，"
                       "不采集麦克风、不接入真实广播链路、不发送任何网络数据。"));
    notice->setWordWrap(true);
    notice->setStyleSheet(QStringLiteral("color: #c9b458; padding: 4px;"));
    root->addWidget(notice);

    setCentralWidget(central);

    connect(startBtn_, &QPushButton::clicked, this, &MainWindow::startEngine);
    connect(stopBtn_, &QPushButton::clicked, this, &MainWindow::stopEngine);
    connect(editBtn_, &QPushButton::clicked, this, &MainWindow::submitEdit);
    connect(undoBtn_, &QPushButton::clicked, this, &MainWindow::undoLast);
    connect(&controller_, &EngineController::logMessage, this,
            &MainWindow::appendLog);
    connect(&controller_, &EngineController::controlResolved, this,
            &MainWindow::onControlResolved);

    uiTimer_.setInterval(50);
    connect(&uiTimer_, &QTimer::timeout, this, &MainWindow::refreshView);
    uiTimer_.start();
}

void MainWindow::startEngine() {
    dbs::EngineConfig cfg;
    cfg.sampleRate = 48000;
    cfg.delayFrames = static_cast<std::int64_t>(cfg.sampleRate) *
                      delayMs_->value() / 1000;
    cfg.capacityFrames = static_cast<std::int64_t>(cfg.sampleRate) *
                         capacityMs_->value() / 1000;
    cfg.fadeFrames = static_cast<std::int64_t>(cfg.sampleRate) *
                     fadeMs_->value() / 1000;
    cfg.underrun = gapMode_->currentIndex() == 1
                       ? dbs::UnderrunPolicy::StopOutput
                       : dbs::UnderrunPolicy::InsertSilence;
    cfg.endOfInput = eoiMode_->currentIndex() == 1
                         ? dbs::EndOfInputPolicy::StopOutput
                         : dbs::EndOfInputPolicy::InsertSilence;
    controller_.start(cfg);
    startBtn_->setEnabled(false);
    stopBtn_->setEnabled(true);
    stateLabel_->setText(QStringLiteral("running"));
    appendLog(QStringLiteral("engine started"));
}

void MainWindow::stopEngine() {
    controller_.stop();
    startBtn_->setEnabled(true);
    stopBtn_->setEnabled(false);
    stateLabel_->setText(QStringLiteral("stopped"));
    appendLog(QStringLiteral("engine stopped"));
}

void MainWindow::submitEdit() {
    bool ok1 = false, ok2 = false;
    const double beginSec = beginSec_->text().toDouble(&ok1);
    const double endSec = endSec_->text().toDouble(&ok2);
    if (!ok1 || !ok2 || endSec <= beginSec) {
        appendLog(QStringLiteral("rejected: invalid time range"));
        return;
    }
    const qint64 begin = static_cast<qint64>(beginSec * 48000);
    const qint64 end = static_cast<qint64>(endSec * 48000);
    controller_.requestEdit(begin, end, padBox_->currentText());
}

void MainWindow::undoLast() {
    if (lastAcceptedEdit_ == 0) {
        appendLog(QStringLiteral("nothing to undo"));
        return;
    }
    controller_.requestUndo(lastAcceptedEdit_);
}

void MainWindow::refreshView() {
    timeline_->setView(controller_.view());
}

void MainWindow::appendLog(const QString& line) {
    logView_->appendPlainText(line);
}

void MainWindow::onControlResolved(quint64 id, const QString& status,
                                   const QString& detail) {
    if (status.startsWith(QStringLiteral("accepted"))) {
        lastAcceptedEdit_ = id;
    }
    appendLog(QStringLiteral("control #%1 %2 %3").arg(id).arg(status, detail));
}

}  // namespace dbsgui
