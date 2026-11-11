#pragma once

#include "engine_controller.h"

#include <QWidget>

namespace dbsgui {

// Paints the bounded ring window: output head (on-air position), write head
// (current input), the controlled in-between region, and every active edit
// with its sequence number. Wall-clock labels are display-only; the marks
// themselves are absolute sample positions.
class TimelineWidget : public QWidget {
    Q_OBJECT
public:
    explicit TimelineWidget(QWidget* parent = nullptr);

    void setView(const EngineView& view);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    EngineView view_;
};

}  // namespace dbsgui
