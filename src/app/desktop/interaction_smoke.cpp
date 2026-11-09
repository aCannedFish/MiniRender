#include "interaction_smoke.hpp"
#include "workbench.hpp"

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QMouseEvent>
#include <QScrollArea>
#include <QTabWidget>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void mouse(VulkanViewport& viewport, QEvent::Type type, QPointF point, Qt::MouseButton button,
           Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QMouseEvent event(type, point, viewport.mapToGlobal(point), button, buttons, modifiers);
    QCoreApplication::sendEvent(&viewport, &event);
}

// 比较同一表单的兄弟字段，滚动内容可以高于 viewport，但不能水平越界或彼此重叠。
void check_form(QScrollArea& area) {
    auto* panel = area.widget();
    std::vector<QWidget*> fields;
    auto* form = qobject_cast<QFormLayout*>(panel->layout());
    require(form != nullptr, "Inspector panel is missing its form layout");
    for (int index = 0; index < form->count(); ++index) {
        auto* widget = form->itemAt(index)->widget();
        if (!widget || !widget->isVisibleTo(panel)) {
            continue;
        }
        const auto rectangle = widget->geometry();
        require(rectangle.left() >= 0 && rectangle.right() < panel->width(),
                "Inspector field exceeds panel width");
        for (auto* other : fields) {
            const auto overlap = rectangle.intersected(other->geometry());
            require(overlap.width() <= 1 || overlap.height() <= 1, "Inspector fields overlap");
        }
        fields.push_back(widget);
    }
    require(fields.size() >= 3, "Inspector did not expose its controls");
    auto* bottom = *std::max_element(fields.begin(), fields.end(), [](QWidget* a, QWidget* b) {
        return a->geometry().bottom() < b->geometry().bottom();
    });
    area.ensureWidgetVisible(bottom);
    const auto center = bottom->mapTo(area.viewport(), bottom->rect().center());
    require(area.viewport()->rect().contains(center),
            "Bottom inspector control cannot be reached by scrolling");
    for (auto* spin : panel->findChildren<QDoubleSpinBox*>()) {
        require(spin->geometry().left() >= 0 &&
                    spin->geometry().right() < spin->parentWidget()->width(),
                "Vector component is clipped by its parent");
    }
    require(panel->width() <= area.viewport()->width(),
            "Inspector requires hidden horizontal scrolling");
}

struct State {
    int tick = 0;
    int heartbeats = 0;
    int gesture = 0;
    int step = 0;
    int frame_at_start = 0;
    int layout_step = 0;
    minirender::Camera before;
    QElapsedTimer clock;
    qint64 previous_tick = 0;
    qint64 maximum_gap = 0;
};

} // namespace

void start_interaction_smoke(Workbench& window, const QString&) {
    const auto state = std::make_shared<State>();
    auto* heartbeat = new QTimer(&window);
    heartbeat->setInterval(10);
    QObject::connect(heartbeat, &QTimer::timeout, &window, [state] { ++state->heartbeats; });
    heartbeat->start();
    auto* timer = new QTimer(&window);
    timer->setInterval(50);
    state->clock.start();
    QObject::connect(timer, &QTimer::timeout, &window, [state, timer, heartbeat, &window] {
        try {
            auto* viewport = window.viewport();
            require(viewport->failure().isEmpty(), "Renderer failed during interaction test");
            require(++state->tick < 300, "Interaction test timed out");
            if (viewport->rendered_frames() < 5) {
                return;
            }
            if (!viewport->can_render()) {
                state->previous_tick = 0;
                return;
            }
            const auto now = state->clock.elapsed();
            if (state->previous_tick) {
                state->maximum_gap = std::max(state->maximum_gap, now - state->previous_tick);
            }
            state->previous_tick = now;
            auto* tabs = window.findChild<QTabWidget*>("inspector_tabs");
            if (state->gesture < 6) {
                const auto button = state->gesture % 3 == 1 ? Qt::RightButton : Qt::LeftButton;
                const auto modifiers = state->gesture % 3 == 2 ? Qt::ShiftModifier : Qt::NoModifier;
                const QPointF origin(viewport->width() * 0.5, viewport->height() * 0.5);
                if (state->step == 0) {
                    tabs->setCurrentIndex(2);
                    state->before = viewport->camera();
                    // 来自其他窗口的按住状态没有本地 press，必须忽略。
                    mouse(*viewport, QEvent::MouseMove, origin, Qt::NoButton, Qt::LeftButton);
                    require(viewport->camera().position == state->before.position,
                            "External press started an unintended drag");
                    state->frame_at_start = viewport->rendered_frames();
                    mouse(*viewport, QEvent::MouseButtonPress, origin, button, button, modifiers);
                } else if (state->step < 9) {
                    mouse(*viewport, QEvent::MouseMove,
                          origin + QPointF(state->step * 4, state->step * 2), Qt::NoButton, button,
                          modifiers);
                } else {
                    mouse(*viewport, QEvent::MouseButtonRelease, origin + QPointF(36, 18), button,
                          Qt::NoButton, modifiers);
                    require(glm::length(viewport->camera().position - state->before.position) +
                                    glm::length(viewport->camera().target - state->before.target) >
                                0.01F,
                            "Drag sequence did not move camera");
                    qInfo() << "Drag" << state->gesture << "frames" << state->frame_at_start << "->"
                            << viewport->rendered_frames() << "exposed" << viewport->isExposed();
                    qInfo() << "Frame state:" << viewport->frame_wait_reason() << "active"
                            << window.isActiveWindow();
                    require(viewport->rendered_frames() > state->frame_at_start + 1,
                            "Rendering stalled during a drag");
                    const auto stopped = viewport->camera();
                    mouse(*viewport, QEvent::MouseMove, origin + QPointF(80, 80), Qt::NoButton,
                          Qt::NoButton);
                    require(viewport->camera().position == stopped.position &&
                                viewport->camera().target == stopped.target,
                            "Camera continued moving after button release");
                    QEvent lost(QEvent::UngrabMouse);
                    QCoreApplication::sendEvent(viewport, &lost);
                    ++state->gesture;
                    state->step = -1;
                }
                ++state->step;
                return;
            }
            if (state->layout_step == 0) {
                const QPointF origin(viewport->width() * 0.5, viewport->height() * 0.5);
                mouse(*viewport, QEvent::MouseButtonPress, origin, Qt::LeftButton, Qt::LeftButton);
                mouse(*viewport, QEvent::MouseMove, origin + QPointF(10, 5), Qt::NoButton,
                      Qt::LeftButton);
                QEvent lost(QEvent::UngrabMouse);
                QCoreApplication::sendEvent(viewport, &lost);
                const auto canceled = viewport->camera();
                mouse(*viewport, QEvent::MouseMove, origin + QPointF(40, 20), Qt::NoButton,
                      Qt::LeftButton);
                require(viewport->camera().position == canceled.position &&
                            viewport->camera().target == canceled.target,
                        "Lost mouse capture did not cancel an active drag");
                mouse(*viewport, QEvent::MouseButtonRelease, origin, Qt::LeftButton, Qt::NoButton);
            }
            if (state->layout_step < 14) {
                const int local_step = state->layout_step % 7;
                if (local_step == 0) {
                    window.resize(state->layout_step < 7 ? 960 : 1180,
                                  state->layout_step < 7 ? 560 : 760);
                }
                if (local_step < 6) {
                    if (local_step % 2 == 0) {
                        tabs->setCurrentIndex(local_step / 2);
                    } else {
                        auto* area = qobject_cast<QScrollArea*>(tabs->currentWidget());
                        require(area != nullptr, "Inspector is missing its scrolling area");
                        check_form(*area);
                    }
                }
                ++state->layout_step;
                return;
            }
            auto* inspector = window.findChild<QDockWidget*>("inspector_dock");
            inspector->hide();
            window.findChild<QAction*>("restore_layout")->trigger();
            require(inspector->isVisible(), "Restore layout did not reopen inspector");
            require(state->heartbeats > 100 && state->maximum_gap < 500,
                    "UI event loop stalled during interaction");
            timer->stop();
            heartbeat->stop();
            qInfo() << "Interaction smoke PASS: 6 drags, release/capture recovery, 2 window sizes, "
                       "inspector restore; max tick gap"
                    << state->maximum_gap << "ms, heartbeats" << state->heartbeats;
            QApplication::exit(0);
        } catch (const std::exception& error) {
            timer->stop();
            heartbeat->stop();
            qCritical() << "Interaction smoke FAIL:" << error.what();
            QApplication::exit(1);
        }
    });
    timer->start();
}
