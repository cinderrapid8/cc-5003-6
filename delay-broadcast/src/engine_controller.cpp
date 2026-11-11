#include "engine_controller.h"

#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

#include <sstream>

using namespace dbs;

namespace dbsgui {

namespace {
QString toQString(const SubmitResult& r) {
    return QString::fromUtf8(toString(r.status));
}
}  // namespace

EngineController::EngineController(QObject* parent) : QObject(parent) {
    // Populate the built-in pad library up front so the UI selector is filled
    // before the first engine start; start() later rebuilds it at the chosen
    // sample rate (which matches the default rate here).
    pads_ = PadLibrary::makeBuiltin(48000);
    sampleRate_ = 48000;
    feedTimer_.setTimerType(Qt::PreciseTimer);
    connect(&feedTimer_, &QTimer::timeout, this, &EngineController::feedTick);
}

EngineController::~EngineController() {
    stop();
}

void EngineController::start(const EngineConfig& cfg) {
    stop();
    sampleRate_ = cfg.sampleRate;
    pads_ = PadLibrary::makeBuiltin(cfg.sampleRate);
    trackCfg_.sampleRate = cfg.sampleRate;
    trackCfg_.blockFrames = 256;
    feedCursor_ = 0;
    blockFrames_ = cfg.sampleRate / 100;  // 10 ms blocks
    feedBuffer_.resize(static_cast<qsizetype>(blockFrames_));

    engine_ = std::make_unique<DelayEngine>(
        cfg, pads_,
        [](const float*, std::int64_t) { /* GUI demo: sink not attached */ },
        blockFrames_);
    engine_->start();

    // Wall-clock timer only paces the demo feed; all engine timing and delay
    // semantics are driven by sample counts inside the core.
    feedTimer_.start(10);
    emit logMessage(QStringLiteral("engine started: delay=%1 ms capacity=%2 ms")
                        .arg(cfg.delayFrames * 1000 / cfg.sampleRate)
                        .arg(cfg.capacityFrames * 1000 / cfg.sampleRate));
}

void EngineController::stop() {
    feedTimer_.stop();
    if (engine_) {
        engine_->signalEndOfInput();
        engine_->stop();
        engine_.reset();
    }
}

QStringList EngineController::padNames() const {
    QStringList names;
    for (const auto& n : pads_.names()) names << QString::fromUtf8(n);
    names.sort();
    return names;
}

void EngineController::feedTick() {
    if (!engine_) return;
    for (qsizetype i = 0; i < feedBuffer_.size(); ++i) {
        feedBuffer_[i] = expectedSampleAt(feedCursor_ + i, trackCfg_);
    }
    engine_->pushInput(feedBuffer_.constData(), blockFrames_);
    feedCursor_ += blockFrames_;
}

void EngineController::requestEdit(qint64 begin, qint64 end, const QString& pad) {
    if (!engine_) return;
    auto fut = engine_->submitEdit(begin, end, pad.toUtf8().constData());
    auto* watcher = new QFutureWatcher<SubmitResult>(this);
    connect(watcher, &QFutureWatcher<SubmitResult>::finished, this, [this, watcher] {
        const SubmitResult r = watcher->result();
        emit controlResolved(r.id, toQString(r),
                             QString::fromUtf8(r.detail.c_str()));
        emit logMessage(QStringLiteral("edit #%1 %2 %3")
                            .arg(r.id)
                            .arg(toQString(r), QString::fromUtf8(r.detail.c_str())));
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([fut = std::move(fut)]() mutable {
        return fut.get();
    }));
}

void EngineController::requestUndo(quint64 id) {
    if (!engine_) return;
    auto fut = engine_->undoEdit(id);
    auto* watcher = new QFutureWatcher<SubmitResult>(this);
    connect(watcher, &QFutureWatcher<SubmitResult>::finished, this, [this, watcher] {
        const SubmitResult r = watcher->result();
        emit controlResolved(r.id, toQString(r),
                             QString::fromUtf8(r.detail.c_str()));
        emit logMessage(QStringLiteral("undo #%1 %2 %3")
                            .arg(r.id)
                            .arg(toQString(r), QString::fromUtf8(r.detail.c_str())));
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([fut = std::move(fut)]() mutable {
        return fut.get();
    }));
}

EngineView EngineController::view() {
    EngineView v;
    if (!engine_) return v;
    const EngineSnapshot s = engine_->snapshot();
    v.writePos = s.writePos;
    v.outputPos = s.outputPos;
    v.buffered = s.bufferedFrames;
    v.capacity = s.capacityFrames;
    v.delay = s.delayFrames;
    v.silenceFilled = s.silenceFilled;
    v.underruns = s.underrunEvents;
    v.backPressure = s.backPressureFrames;
    v.paused = s.paused;
    v.inputEnded = s.inputEnded;
    for (const auto& e : s.edits) {
        v.edits.push_back({e.id, e.seq, e.begin, e.end,
                           QString::fromUtf8(e.padName.c_str())});
    }
    return v;
}

}  // namespace dbsgui
