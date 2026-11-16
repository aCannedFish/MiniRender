// 工作台颜色、尺寸、图标及导航和停靠标题的统一样式。
// 本模块接口见 editor_theme.hpp。

#include "editor_theme.hpp"

#include <QApplication>
#include <QButtonGroup>
#include <QDockWidget>
#include <QFile>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <array>
#include <stdexcept>

namespace editor {
namespace {

// Fusion 提供一致的焦点、命中区和键盘行为；尺寸取逻辑像素，交给 Qt 处理 DPI。
class EditorStyle final : public QProxyStyle {
  public:
    EditorStyle() : QProxyStyle(QStyleFactory::create("Fusion")) {}

    int pixelMetric(PixelMetric metric, const QStyleOption* option,
                    const QWidget* widget) const override {
        switch (metric) {
        case PM_LayoutLeftMargin:
        case PM_LayoutRightMargin:
        case PM_LayoutTopMargin:
        case PM_LayoutBottomMargin:
            return 10;
        case PM_LayoutHorizontalSpacing:
            return 8;
        case PM_LayoutVerticalSpacing:
            return 6;
        case PM_SmallIconSize:
            return 16;
        case PM_ToolBarIconSize:
            return 16;
        case PM_DockWidgetSeparatorExtent:
            return 3;
        default:
            return QProxyStyle::pixelMetric(metric, option, widget);
        }
    }
};

// 所有路径使用 16×16 坐标。选中、禁用和高 DPI 版本使用相同轮廓。
void draw_icon(QPainter& p, Icon kind) {
    auto line = [&](qreal a, qreal b, qreal c, qreal d) {
        p.drawLine(QPointF(a, b), QPointF(c, d));
    };
    auto path = [&](std::initializer_list<QPointF> points) {
        QPainterPath shape;
        bool         first = true;
        for (auto point : points) {
            if (first) {
                shape.moveTo(point);
            } else {
                shape.lineTo(point);
            }
            first = false;
        }
        p.drawPath(shape);
    };
    switch (kind) {
    case Icon::open:
        path({{2, 12}, {2, 4}, {6, 4}, {7, 6}, {14, 6}, {12, 12}, {2, 12}, {4, 7}, {13, 7}});
        break;
    case Icon::save:
    case Icon::save_as:
        path({{3, 2}, {11, 2}, {13, 4}, {13, 14}, {3, 14}, {3, 2}});
        p.drawRect(QRectF(5, 2, 5, 4));
        p.drawRect(QRectF(5, 9, 6, 5));
        if (kind == Icon::save_as) {
            line(10, 12, 15, 7);
            line(13, 6, 15, 8);
        }
        break;
    case Icon::undo:
    case Icon::redo:
        if (kind == Icon::redo) {
            p.translate(16, 0);
            p.scale(-1, 1);
        }
        path({{5, 3}, {2, 6}, {5, 9}});
        {
            QPainterPath curve;
            curve.moveTo(2, 6);
            curve.lineTo(9, 6);
            curve.cubicTo(15, 6, 15, 13, 8, 13);
            p.drawPath(curve);
        }
        break;
    case Icon::import_mesh:
        path({{2, 6}, {2, 13}, {14, 13}, {14, 6}});
        line(8, 2, 8, 10);
        path({{5, 7}, {8, 10}, {11, 7}});
        break;
    case Icon::texture:
        p.drawRect(QRectF(2, 2, 12, 12));
        line(2, 6, 14, 6);
        line(2, 10, 14, 10);
        line(6, 2, 6, 14);
        line(10, 2, 10, 14);
        break;
    case Icon::duplicate:
        p.drawRect(QRectF(5, 5, 9, 9));
        path({{2, 11}, {2, 2}, {11, 2}});
        break;
    case Icon::remove:
        line(3, 4, 13, 4);
        path({{4, 4}, {5, 14}, {11, 14}, {12, 4}});
        path({{6, 4}, {6, 2}, {10, 2}, {10, 4}});
        line(7, 7, 7, 11);
        line(9, 7, 9, 11);
        break;
    case Icon::camera:
        path({{2, 5}, {5, 5}, {6, 3}, {10, 3}, {11, 5}, {14, 5}, {14, 13}, {2, 13}, {2, 5}});
        p.drawEllipse(QPointF(8, 9), 2.5, 2.5);
        break;
    case Icon::export_image:
        path({{6, 2}, {2, 2}, {2, 14}, {14, 14}, {14, 10}});
        path({{8, 2}, {14, 2}, {14, 8}});
        line(14, 2, 7, 9);
        break;
    case Icon::layout:
        p.drawRect(QRectF(2, 2, 12, 12));
        line(2, 5, 14, 5);
        line(5, 5, 5, 14);
        line(11, 5, 11, 14);
        break;
    case Icon::select:
        path({{3, 2}, {4, 13}, {7, 10}, {10, 14}, {12, 13}, {9, 9}, {13, 8}, {3, 2}});
        break;
    case Icon::move:
        line(8, 1, 8, 15);
        line(1, 8, 15, 8);
        path({{5, 4}, {8, 1}, {11, 4}});
        path({{5, 12}, {8, 15}, {11, 12}});
        path({{4, 5}, {1, 8}, {4, 11}});
        path({{12, 5}, {15, 8}, {12, 11}});
        break;
    case Icon::rotate:
        p.drawArc(QRectF(3, 3, 10, 10), 40 * 16, 280 * 16);
        path({{9, 1}, {12, 4}, {8, 5}});
        break;
    case Icon::cube:
        path({{8, 1}, {14, 4}, {14, 12}, {8, 15}, {2, 12}, {2, 4}, {8, 1}});
        path({{2, 4}, {8, 7}, {14, 4}});
        line(8, 7, 8, 15);
        break;
    case Icon::close:
        line(4, 4, 12, 12);
        line(4, 12, 12, 4);
        break;
    case Icon::settings:
        line(2, 4, 14, 4);
        line(2, 8, 14, 8);
        line(2, 12, 14, 12);
        p.setBrush(QColor("#1c1f25"));
        p.drawEllipse(QPointF(6, 4), 1.5, 1.5);
        p.drawEllipse(QPointF(10, 8), 1.5, 1.5);
        p.drawEllipse(QPointF(5, 12), 1.5, 1.5);
        break;
    case Icon::undock:
        p.drawRect(QRectF(2, 5, 9, 9));
        path({{5, 3}, {5, 2}, {14, 2}, {14, 11}, {13, 11}});
        break;
    }
}
} // namespace

QIcon icon(Icon kind) {
    QIcon result;
    for (int scale : {1, 2, 3}) {
        for (auto mode : {QIcon::Normal, QIcon::Disabled, QIcon::Active}) {
            QPixmap pixmap(16 * scale, 16 * scale);
            pixmap.setDevicePixelRatio(scale);
            pixmap.fill(Qt::transparent);
            QPainter p(&pixmap);
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(QPen(mode == QIcon::Disabled ? QColor("#626975") : QColor("#c7ced9"), 1.25,
                          Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            draw_icon(p, kind);
            p.end();
            result.addPixmap(pixmap, mode);
        }
    }
    return result;
}

void apply_theme(QApplication& application) {
    application.setStyle(new EditorStyle);
    QFont font = application.font();
    font.setPixelSize(13);
    application.setFont(font);
    QPalette palette;
    palette.setColor(QPalette::Light, QColor("#424a57"));
    palette.setColor(QPalette::Midlight, QColor("#343b46"));
    palette.setColor(QPalette::Dark, QColor("#171b21"));
    palette.setColor(QPalette::Mid, QColor("#353c48"));
    palette.setColor(QPalette::Shadow, QColor("#111419"));
    palette.setColor(QPalette::Window, QColor("#22252b"));
    palette.setColor(QPalette::WindowText, QColor("#dce1e9"));
    palette.setColor(QPalette::Base, QColor("#191c22"));
    palette.setColor(QPalette::AlternateBase, QColor("#242830"));
    palette.setColor(QPalette::Text, QColor("#dce1e9"));
    palette.setColor(QPalette::Button, QColor("#30353e"));
    palette.setColor(QPalette::ButtonText, QColor("#dce1e9"));
    palette.setColor(QPalette::Highlight, QColor("#315982"));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::ToolTipBase, QColor("#30353e"));
    palette.setColor(QPalette::ToolTipText, QColor("#edf1f7"));
    palette.setColor(QPalette::PlaceholderText, QColor("#858e9d"));
    for (auto role : {QPalette::Text, QPalette::ButtonText, QPalette::WindowText}) {
        palette.setColor(QPalette::Disabled, role, QColor("#717987"));
    }
    application.setPalette(palette);
    QFile sheet(":/editor/editor.qss");
    if (!sheet.open(QIODevice::ReadOnly)) {
        throw std::runtime_error("Editor stylesheet is missing");
    }
    application.setStyleSheet(QString::fromUtf8(sheet.readAll()));
}

QLabel* section(const QString& title, QWidget* parent) {
    auto* label = new QLabel(title, parent);
    label->setProperty("role", "section");
    return label;
}

QWidget* inspector_navigation(QTabWidget* tabs, const std::vector<int>& page_ids,
                              const std::function<void(int)>& activate) {
    auto* container = new QWidget;
    auto* layout    = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* navigation = new QWidget(container);
    navigation->setObjectName(tabs->objectName() + "_navigation");
    navigation->setProperty("role", "panelNavigation");
    auto* grid = new QGridLayout(navigation);
    grid->setContentsMargins(8, 6, 8, 7);
    grid->setSpacing(3);
    auto* group = new QButtonGroup(navigation);
    group->setExclusive(true);
    for (int i = 0; i < tabs->count(); ++i) {
        auto* button = new QToolButton(navigation);
        button->setText(tabs->tabText(i));
        button->setObjectName(
            QString("inspector_page_%1").arg(page_ids[static_cast<std::size_t>(i)]));
        button->setCheckable(true);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setFocusPolicy(Qt::StrongFocus);
        group->addButton(button, i);
        grid->addWidget(button, i / 3, i % 3);
        grid->setColumnStretch(i % 3, 1);
    }
    group->button(tabs->currentIndex())->setChecked(true);
    QObject::connect(group, &QButtonGroup::idClicked, tabs, [page_ids, activate](int index) {
        activate(page_ids[static_cast<std::size_t>(index)]);
    });
    QObject::connect(tabs, &QTabWidget::currentChanged, group, [group](int index) {
        if (auto* b = group->button(index)) {
            b->setChecked(true);
        }
    });
    tabs->tabBar()->hide();
    layout->addWidget(navigation);
    layout->addWidget(tabs, 1);
    return container;
}

void decorate_dock(QDockWidget* dock) {
    auto* title = new QWidget(dock);
    title->setObjectName("dock_heading");
    auto* layout = new QHBoxLayout(title);
    layout->setContentsMargins(10, 3, 5, 3);
    layout->setSpacing(3);
    auto* label = new QLabel(dock->windowTitle(), title);
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    label->setProperty("role", "dockTitle");
    layout->addWidget(label);
    layout->addStretch();
    auto* undock = new QToolButton(title);
    undock->setIcon(icon(Icon::undock));
    undock->setToolTip("浮动 / 停靠面板");
    undock->setAccessibleName(undock->toolTip());
    auto* close = new QToolButton(title);
    close->setIcon(icon(Icon::close));
    close->setToolTip("关闭面板（工具栏可恢复布局）");
    close->setAccessibleName("关闭面板");
    layout->addWidget(undock);
    layout->addWidget(close);
    QObject::connect(undock, &QToolButton::clicked, dock,
                     [dock] { dock->setFloating(!dock->isFloating()); });
    QObject::connect(close, &QToolButton::clicked, dock, &QWidget::hide);
    dock->setTitleBarWidget(title);
}
} // namespace editor
