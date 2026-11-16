#include "theme_smoke.hpp"
#include "geometry_smoke.hpp"
#include "workbench.hpp"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QMouseEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyleOptionSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>

#include <array>
#include <memory>
#include <stdexcept>

namespace {
void require(bool condition, const char* error) {
    if (!condition) {
        throw std::runtime_error(error);
    }
}

// 检查同一布局中的控件边界。不同父级可以重叠，例如输入框内部的文本编辑器。
void check_layout(QWidget* panel) {
    for (auto* layout : panel->findChildren<QLayout*>()) {
        std::vector<QRect> rectangles;
        for (int i = 0; i < layout->count(); ++i) {
            auto* widget = layout->itemAt(i)->widget();
            if (!widget || !widget->isVisibleTo(panel)) {
                continue;
            }
            auto*      parent = widget->parentWidget();
            const auto r      = widget->geometry();
            if (r.left() < 0 || r.right() >= parent->width()) {
                qCritical() << "Clipped field" << widget->objectName() << r << parent->size();
                throw std::runtime_error("Theme clipped an inspector field");
            }
            for (const auto& other : rectangles) {
                const auto overlap = r.intersected(other);
                require(overlap.width() <= 1 || overlap.height() <= 1,
                        "Theme caused overlapping fields");
            }
            rectangles.push_back(r);
        }
    }
}

// 对实际箭头命中区发送鼠标事件，检查样式更换后步进按钮仍然可用。
void check_stepper(QDoubleSpinBox* spin) {
    require(spin && spin->isEnabled(), "Missing enabled transform field");
    QStyleOptionSpinBox option;
    option.initFrom(spin);
    option.rect          = spin->rect();
    option.frame         = true;
    option.buttonSymbols = spin->buttonSymbols();
    option.stepEnabled   = QAbstractSpinBox::StepUpEnabled | QAbstractSpinBox::StepDownEnabled;
    const auto rect =
        spin->style()->subControlRect(QStyle::CC_SpinBox, &option, QStyle::SC_SpinBoxUp, spin);
    require(!rect.isEmpty() && spin->rect().contains(rect), "Spin button is outside its field");
    const double  before   = spin->value();
    const QPointF position = rect.center();
    const QPointF global   = spin->mapToGlobal(rect.center());
    QMouseEvent   press(QEvent::MouseButtonPress, position, position, global, Qt::LeftButton,
                        Qt::LeftButton, Qt::NoModifier);
    QMouseEvent   release(QEvent::MouseButtonRelease, position, position, global, Qt::LeftButton,
                          Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(spin, &press);
    QCoreApplication::sendEvent(spin, &release);
    require(spin->value() > before, "Styled spin button did not increment its value");
    spin->setValue(before);
    spin->clearFocus();
    spin->findChild<QLineEdit*>()->deselect();
}

struct State {
    int tick = 0, stage = 0;
};
} // namespace

void start_theme_smoke(Workbench& window, const QString& directory) {
    require(QDir().mkpath(directory), "Cannot create theme capture directory");
    window.select_object("blue_cube");
    auto  state = std::make_shared<State>();
    auto* timer = new QTimer(&window);
    timer->setInterval(80);
    QObject::connect(timer, &QTimer::timeout, &window, [state, timer, &window, directory] {
        try {
            require(++state->tick < 500, "Theme check timed out");
            require(window.viewport()->failure().isEmpty(), "Theme viewport failed");
            if (!window.viewport()->can_render() || window.viewport()->rendered_frames() < 5) {
                return;
            }
            auto*                          tabs = window.panels();
            constexpr std::array<QSize, 3> sizes{QSize(1280, 800), QSize(960, 560),
                                                 QSize(1100, 700)};
            const int                      case_id = state->stage / 4;
            if (case_id >= tabs->count() * int(sizes.size())) {
                const int object_page =
                    window.findChild<QTabWidget*>("object_tabs")->currentIndex();
                auto* toggle = window.findChild<QAction*>("toggle_render_dock");
                toggle->trigger();
                require(!window.panels()->sceneDock()->isVisible() &&
                            window.panels()->objectDock()->isVisible(),
                        "Render dock toggle hid the object inspector");
                toggle->trigger();
                window.panels()->objectDock()->hide();
                window.findChild<QAction*>("restore_layout")->trigger();
                require(window.panels()->objectDock()->isVisible() &&
                            window.panels()->sceneDock()->isVisible(),
                        "Layout restore did not reopen both docks");
                require(window.findChild<QTabWidget*>("object_tabs")->currentIndex() == object_page,
                        "Panel visibility reset the object page");
                timer->stop();
                qInfo() << "Theme smoke PASS: all 12 pages, 3 window sizes, navigation, field "
                           "bounds, scrolling, stepper and captures";
                QApplication::exit(0);
                return;
            }
            const auto size = sizes[static_cast<std::size_t>(case_id / tabs->count())];
            const int  page = case_id % tabs->count();
            auto* button = window.findChild<QToolButton*>(QString("inspector_page_%1").arg(page));
            require(button && button->isVisibleTo(&window), "Inspector page is not accessible");
            if (state->stage % 4 == 0) {
                window.resize(size);
                const bool object_page = page == 0 || page == 6 || page == 9;
                auto*      other_tabs =
                    window.findChild<QTabWidget*>(object_page ? "scene_tabs" : "object_tabs");
                const int other_page = other_tabs->currentIndex();
                button->click();
                require(other_tabs->currentIndex() == other_page,
                        "Switching one dock changed the other dock");
                require(window.panels()->objectDock()->isVisible() &&
                            window.panels()->sceneDock()->isVisible(),
                        "Independent docks are not visible");
                require(tabs->currentIndex() == page && button->isChecked(),
                        "Inspector navigation failed");
            } else if (state->stage % 4 == 2) {
                require(window.size() == size, "Theme forced a larger window");
                auto* area = qobject_cast<QScrollArea*>(tabs->currentWidget());
                require(area, "Missing inspector scroll area");
                if (area->widget()->width() > area->viewport()->width()) {
                    qCritical() << "Panel overflow" << page << size << area->widget()->size()
                                << area->viewport()->size() << area->widget()->minimumSizeHint();
                    for (auto* field : area->widget()->findChildren<QWidget*>(
                             QString(), Qt::FindDirectChildrenOnly)) {
                        if (field->isVisibleTo(area->widget())) {
                            qCritical()
                                << field->objectName() << field->size() << field->minimumSizeHint();
                        }
                    }
                    capture_workbench_ui(window, directory + "/overflow.png");
                    throw std::runtime_error("Theme requires hidden horizontal scrolling");
                }
                check_layout(area->widget());
                auto* navigation = window.findChild<QWidget*>("scene_tabs_navigation");
                check_layout(navigation);
                if (page == 0) {
                    check_stepper(window.findChild<QDoubleSpinBox*>("translation.x"));
                }
                area->verticalScrollBar()->setValue(0);
                if (page == 0 || page == 3 || page == 6 || page == 11) {
                    capture_workbench_ui(
                        window,
                        directory + QString("/workbench-%1-%2.png").arg(size.width()).arg(page));
                }
                area->verticalScrollBar()->setValue(area->verticalScrollBar()->maximum());
            } else if (state->stage % 4 == 3) {
                auto* area = qobject_cast<QScrollArea*>(tabs->currentWidget());
                require(area->verticalScrollBar()->value() == area->verticalScrollBar()->maximum(),
                        "Inspector did not reach bottom");
                qInfo() << "Theme layout" << size << tabs->tabText(page) << "DPR"
                        << window.devicePixelRatioF();
            }
            ++state->stage;
        } catch (const std::exception& e) {
            timer->stop();
            qCritical() << "Theme smoke FAIL:" << e.what();
            QApplication::exit(1);
        }
    });
    timer->start();
}
