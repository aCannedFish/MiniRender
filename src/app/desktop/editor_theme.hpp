// 工作台颜色、尺寸、图标及导航和停靠标题的统一样式。

#pragma once

#include <QIcon>
#include <functional>
#include <vector>

class QApplication;
class QDockWidget;
class QLabel;
class QTabWidget;
class QWidget;

namespace editor {

// 工作台共用的线形图标。由 QPainter 生成多分辨率图像，安装包无需图标字体或 SVG 插件。
enum class Icon {
    open,
    save,
    save_as,
    undo,
    redo,
    import_mesh,
    texture,
    duplicate,
    remove,
    camera,
    export_image,
    layout,
    select,
    move,
    rotate,
    cube,
    close,
    settings,
    undock
};
QIcon icon(Icon kind);

// 在创建窗口前设置跨平台样式、调色板与控件样式表。
void apply_theme(QApplication& application);

// 紧凑导航映射到各组 QTabWidget，页面编号由工作台路由器提供。
QWidget* inspector_navigation(QTabWidget* tabs, const std::vector<int>& page_ids,
                              const std::function<void(int)>& activate);
void decorate_dock(QDockWidget* dock);
QLabel* section(const QString& title, QWidget* parent);

} // namespace editor
