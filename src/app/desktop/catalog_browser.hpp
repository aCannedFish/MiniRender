// 可搜索的资源选择器。调用方负责把选中的条目交给场景编辑器或序列查看器。
#pragma once
#include "resource_catalog.hpp"
#include <QDialog>

class QLabel;
class QLineEdit;
class QComboBox;
class QListWidget;
class QPushButton;

// 浏览随应用发布的示例和用户挂载的一个课程包。挂载路径保存在应用设置中。
class CatalogBrowser final : public QDialog {
  public:
    explicit CatalogBrowser(QWidget* parent);
    // 成功解析整份索引后才替换当前课程包。
    bool mount(const QString& directory);
    std::function<void(const catalog::Entry&, bool recommended)> on_open;

  private:
    // 重新应用文本与类型筛选，并选中第一个匹配条目。
    void filter();
    // 根据当前条目显示来源、摘要、操作名称和说明。
    void describe();
    QList<catalog::Entry> builtin_, course_;
    QLineEdit*            search_;
    QComboBox*            type_;
    QListWidget*          list_;
    QLabel*               detail_;
    QLabel*               notice_;
    QPushButton*          open_;
    QPushButton*          recommended_;
};
