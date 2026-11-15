#include "workbench_panels.hpp"
#include "editor_theme.hpp"

#include <QDockWidget>
#include <QMainWindow>
#include <QTabWidget>

WorkbenchPanels::WorkbenchPanels(QMainWindow& window) : QObject(&window), window_(window) {}

void WorkbenchPanels::addTab(QWidget* widget, const QString& title) {
    pages_.push_back({widget, title});
}

void WorkbenchPanels::build(QDockWidget* scene_tree) {
    tree_dock_ = scene_tree;
    object_dock_ = new QDockWidget("物体检查器", &window_);
    object_dock_->setObjectName("inspector_dock");
    object_dock_->setMinimumWidth(340);
    object_dock_->setMaximumWidth(620);
    scene_dock_ = new QDockWidget("场景与渲染", &window_);
    scene_dock_->setObjectName("render_dock");
    scene_dock_->setMinimumWidth(320);
    scene_dock_->setMaximumWidth(620);
    auto* object_tabs = new QTabWidget;
    object_tabs->setObjectName("object_tabs");
    auto* scene_tabs = new QTabWidget;
    scene_tabs->setObjectName("scene_tabs");
    std::vector<int> object_ids, scene_ids;
    for (int index = 0; index < count(); ++index) {
        auto& page = pages_[static_cast<std::size_t>(index)];
        const bool object = index == 0 || index == 6 || index == 9;
        page.tabs = object ? object_tabs : scene_tabs;
        page.local_index = page.tabs->addTab(page.widget, page.title);
        (object ? object_ids : scene_ids).push_back(index);
    }
    const auto activate = [this](int page) { setCurrentIndex(page); };
    object_dock_->setWidget(editor::inspector_navigation(object_tabs, object_ids, activate));
    scene_dock_->setWidget(editor::inspector_navigation(scene_tabs, scene_ids, activate));
    for (auto* dock : {object_dock_, scene_dock_}) {
        editor::decorate_dock(dock);
    }
    for (auto* tabs : {object_tabs, scene_tabs}) {
        connect(tabs, &QTabWidget::currentChanged, this, [this, tabs](int local) {
            for (int i = 0; i < count(); ++i) {
                const auto& page = pages_[static_cast<std::size_t>(i)];
                if (page.tabs == tabs && page.local_index == local) {
                    current_ = i;
                    if (on_current_changed) {
                        on_current_changed(i);
                    }
                    break;
                }
            }
        });
    }
    scene_tabs->setCurrentIndex(pages_[4].local_index);
    current_ = 0;
    restore();
}

void WorkbenchPanels::restore() {
    for (auto* dock : {tree_dock_, scene_dock_, object_dock_}) {
        dock->setFloating(false);
    }
    window_.addDockWidget(Qt::LeftDockWidgetArea, tree_dock_);
    window_.splitDockWidget(tree_dock_, scene_dock_, Qt::Vertical);
    window_.addDockWidget(Qt::RightDockWidgetArea, object_dock_);
    for (auto* dock : {tree_dock_, scene_dock_, object_dock_}) {
        dock->show();
    }
    window_.resizeDocks({tree_dock_, object_dock_}, {320, 360}, Qt::Horizontal);
    window_.resizeDocks({tree_dock_, scene_dock_}, {170, 490}, Qt::Vertical);
}

void WorkbenchPanels::setCurrentIndex(int page) {
    auto& target = pages_.at(static_cast<std::size_t>(page));
    const bool changed = target.tabs->currentIndex() != target.local_index;
    target.tabs->setCurrentIndex(target.local_index);
    auto* dock = (page == 0 || page == 6 || page == 9) ? object_dock_ : scene_dock_;
    dock->show();
    dock->raise();
    current_ = page;
    if (!changed && on_current_changed) {
        on_current_changed(page);
    }
}
int WorkbenchPanels::currentIndex() const noexcept {
    return current_;
}
int WorkbenchPanels::count() const noexcept {
    return static_cast<int>(pages_.size());
}
QWidget* WorkbenchPanels::widget(int page) const {
    return pages_.at(static_cast<std::size_t>(page)).widget;
}
QWidget* WorkbenchPanels::currentWidget() const {
    return widget(current_);
}
QDockWidget* WorkbenchPanels::objectDock() const noexcept {
    return object_dock_;
}
QDockWidget* WorkbenchPanels::sceneDock() const noexcept {
    return scene_dock_;
}

QString WorkbenchPanels::tabText(int page) const {
    return pages_.at(static_cast<std::size_t>(page)).title;
}
