// 物体与场景面板的页面路由、独立停靠和布局恢复。

#pragma once

#include <QObject>
#include <QString>
#include <functional>
#include <vector>

class QDockWidget;
class QMainWindow;
class QTabWidget;
class QWidget;

// 管理独立的物体检查器与场景设置面板。页面编号由注册顺序确定，切换一组时保留另一组。
// 对象相关页面为物体、几何和布料；灯光、相机、渲染及独立工具使用场景面板。
class WorkbenchPanels final : public QObject {
  public:
    explicit WorkbenchPanels(QMainWindow& window);
    void addTab(QWidget* widget, const QString& title);
    void build(QDockWidget* scene_tree);
    void restore();
    void setCurrentIndex(int page);
    int currentIndex() const noexcept;
    int count() const noexcept;
    QWidget* widget(int page) const;
    QString tabText(int page) const;
    QWidget* currentWidget() const;
    QDockWidget* objectDock() const noexcept;
    QDockWidget* sceneDock() const noexcept;
    std::function<void(int)> on_current_changed;

  private:
    // 将页面标题、控件与所属标签栏关联，供分类和恢复导航。
    struct Page {
        QWidget*    widget;
        QString     title;
        QTabWidget* tabs        = nullptr;
        int         local_index = 0;
    };

    QMainWindow&      window_;
    std::vector<Page> pages_;
    QDockWidget*      object_dock_ = nullptr;
    QDockWidget*      scene_dock_  = nullptr;
    QDockWidget*      tree_dock_   = nullptr;
    int               current_     = 0;
};
