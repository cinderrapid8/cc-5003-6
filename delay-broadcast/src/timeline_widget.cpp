#include "timeline_widget.h"

#include <QPainter>
#include <QPainterPath>
#include <QtMath>

namespace dbsgui {

namespace {
QString msLabel(qint64 frames, int rate) {
    const double seconds = static_cast<double>(frames) / rate;
    return QString::number(seconds, 'f', 2) + QStringLiteral(" s");
}
}  // namespace

TimelineWidget::TimelineWidget(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(220);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void TimelineWidget::setView(const EngineView& view) {
    view_ = view;
    update();
}

void TimelineWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = rect().adjusted(20, 20, -20, -40);

    p.fillRect(rect(), QColor(0x1e, 0x1f, 0x24));

    // Full strip represents the bounded ring capacity; the brighter segment
    // is the currently controlled (buffered, not yet aired) region.
    p.setPen(QPen(QColor(0x3a, 0x3d, 0x46), 1));
    p.setBrush(QColor(0x2a, 0x2d, 0x35));
    p.drawRoundedRect(r, 6, 6);

    const qint64 span = std::max<qint64>(view_.capacity, 1);
    const qint64 windowStart = std::max<qint64>(0, view_.writePos - span);
    auto xOf = [&](qint64 frame) -> qreal {
        return r.left() +
               static_cast<qreal>(frame - windowStart) / span * r.width();
    };

    const qreal outX = xOf(view_.outputPos);
    const qreal writeX = xOf(view_.writePos);

    p.setBrush(QColor(0x35, 0x4a, 0x6b));
    p.setPen(Qt::NoPen);
    if (writeX > outX) {
        p.drawRect(QRectF(outX, r.top() + 8, writeX - outX, r.height() - 16));
    }

    // Edits: colored blocks labelled with id/seq, showing which edit wins
    // overlaps (the block with the highest sequence on that frame).
    int colorIndex = 0;
    for (const EditView& e : view_.edits) {
        const qreal x1 = xOf(e.begin);
        const qreal x2 = xOf(e.end);
        QColor base = QColor::fromHsv((colorIndex++ * 67 + 15) % 360, 180, 220);
        p.setBrush(base);
        p.setOpacity(0.85);
        const QRectF er(x1, r.top() + 26, std::max<qreal>(2, x2 - x1), 44);
        p.drawRoundedRect(er, 4, 4);
        p.setOpacity(1.0);
        p.setPen(Qt::black);
        QFont small = p.font();
        small.setPointSize(8);
        p.setFont(small);
        p.drawText(er.adjusted(3, 2, -3, -2),
                   Qt::AlignLeft | Qt::AlignTop,
                   QStringLiteral("#%1 s%2\n%3")
                       .arg(e.id)
                       .arg(e.seq)
                       .arg(e.pad));
        p.setPen(QPen(QColor(0x3a, 0x3d, 0x46), 1));
    }

    // Output head (on-air) and write head (input).
    auto drawHead = [&](qreal x, const QColor& color, const QString& tag,
                        const QString& value) {
        p.setPen(QPen(color, 2));
        p.drawLine(QPointF(x, r.top() - 6), QPointF(x, r.bottom() + 6));
        QPolygon tri;
        tri << QPoint(x - 6, r.top() - 6) << QPoint(x + 6, r.top() - 6)
            << QPoint(x, r.top() - 16);
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawPolygon(tri);
        p.setPen(color);
        QFont f = p.font();
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRectF(x - 70, r.bottom() + 8, 140, 26),
                   Qt::AlignCenter, tag + QStringLiteral("\n") + value);
    };
    drawHead(outX, QColor(0x66, 0xff, 0x99),
             QStringLiteral("OUTPUT / on-air"),
             msLabel(view_.outputPos, 48000));
    drawHead(writeX, QColor(0x66, 0xb3, 0xff),
             QStringLiteral("INPUT head"),
             msLabel(view_.writePos, 48000));

    // Top-left numeric readout (display-only timing).
    p.setPen(QColor(0xdd, 0xdd, 0xdd));
    QFont mono = p.font();
    mono.setFamily(QStringLiteral("Monospace"));
    mono.setPointSize(9);
    p.setFont(mono);
    const QString info =
        QStringLiteral("buffered=%1 frames (%2)   silence=%3   underruns=%4   "
                       "back-pressure=%5%6")
            .arg(view_.buffered)
            .arg(msLabel(view_.buffered, 48000))
            .arg(view_.silenceFilled)
            .arg(view_.underruns)
            .arg(view_.backPressure)
            .arg(view_.paused ? QStringLiteral("   [PAUSED: stop-output]")
                              : (view_.inputEnded ? QStringLiteral("   [EOS]")
                                                  : QString()));
    p.drawText(QRectF(r.left(), 2, r.width(), 16), Qt::AlignLeft | Qt::AlignVCenter,
               info);
}

}  // namespace dbsgui
