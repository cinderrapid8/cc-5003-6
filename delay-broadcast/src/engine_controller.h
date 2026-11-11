#pragma once

#include "../src/delay_engine.h"
#include "../src/pads.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QTimer>
#include <memory>

namespace dbsgui {

struct EditView {
    quint64 id = 0;
    quint64 seq = 0;
    qint64 begin = 0;
    qint64 end = 0;
    QString pad;
};

struct EngineView {
    qint64 writePos = 0;
    qint64 outputPos = 0;
    qint64 buffered = 0;
    qint64 capacity = 0;
    qint64 delay = 0;
    qint64 silenceFilled = 0;
    qint64 underruns = 0;
    qint64 backPressure = 0;
    bool paused = false;
    bool inputEnded = false;
    QVector<EditView> edits;
};

// Owns the threaded engine and feeds it a local code-track generator on a
// timer. The worker thread never waits for the UI; control calls return
// futures whose results are marshalled back through Qt signals.
class EngineController : public QObject {
    Q_OBJECT
public:
    explicit EngineController(QObject* parent = nullptr);
    ~EngineController() override;

    void start(const dbs::EngineConfig& cfg);
    void stop();

    dbs::PadLibrary& pads() { return pads_; }
    QStringList padNames() const;

    void requestEdit(qint64 begin, qint64 end, const QString& pad);
    void requestUndo(quint64 id);

    EngineView view();
    int sampleRate() const { return sampleRate_; }

signals:
    void controlResolved(quint64 id, const QString& status, const QString& detail);
    void logMessage(const QString& line);

private:
    void feedTick();

    dbs::PadLibrary pads_;
    std::unique_ptr<dbs::DelayEngine> engine_;
    QTimer feedTimer_;
    dbs::CodeTrackConfig trackCfg_;
    int sampleRate_ = 48000;
    qint64 feedCursor_ = 0;
    qint64 blockFrames_ = 480;
    QVector<float> feedBuffer_;
};

}  // namespace dbsgui
