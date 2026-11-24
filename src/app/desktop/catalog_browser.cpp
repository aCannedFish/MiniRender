// 资源浏览窗口。内置示例和课程包共享搜索、类型过滤和导入操作。
#include "catalog_browser.hpp"
#include "builtin_scenes.hpp"
#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

CatalogBrowser::CatalogBrowser(QWidget* parent) : QDialog(parent) {
    setWindowTitle("资源目录");
    setObjectName("resource_browser");
    setAttribute(Qt::WA_DeleteOnClose);
    resize(600, 520);
    auto* layout = new QVBoxLayout(this);
    auto* bar    = new QHBoxLayout;
    layout->addLayout(bar);
    search_ = new QLineEdit;
    search_->setPlaceholderText("搜索场景、模型、序列或作业编号");
    type_ = new QComboBox;
    type_->addItems({"全部类型", "场景", "模型", "帧序列"});
    auto* mount_button = new QPushButton("选择课程包…");
    bar->addWidget(search_, 1);
    bar->addWidget(type_);
    bar->addWidget(mount_button);
    list_ = new QListWidget;
    list_->setIconSize(QSize(80, 60));
    list_->setUniformItemSizes(true);
    layout->addWidget(list_, 1);
    detail_ = new QLabel;
    detail_->setWordWrap(true);
    detail_->setTextFormat(Qt::PlainText);
    detail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(detail_);
    notice_ = new QLabel;
    notice_->setTextFormat(Qt::PlainText);
    notice_->setWordWrap(true);
    layout->addWidget(notice_);
    open_         = new QPushButton;
    auto* actions = new QHBoxLayout;
    recommended_  = new QPushButton("覆盖并预览 Blinn–Phong");
    actions->addStretch();
    actions->addWidget(recommended_);
    actions->addWidget(open_);
    layout->addLayout(actions);
    connect(search_, &QLineEdit::textChanged, this, [this] { filter(); });
    connect(type_, &QComboBox::currentIndexChanged, this, [this] { filter(); });
    connect(list_, &QListWidget::currentRowChanged, this, [this] { describe(); });
    const auto activate = [this](bool recommended) {
        auto* item = list_->currentItem();
        if (!item || !on_open) {
            return;
        }
        const int index = item->data(Qt::UserRole).toInt();
        auto      entry =
            index < builtin_.size() ? builtin_.at(index) : course_.at(index - builtin_.size());
        accept();
        on_open(entry, recommended);
    };
    connect(open_, &QPushButton::clicked, this, [activate] { activate(false); });
    connect(recommended_, &QPushButton::clicked, this, [activate] { activate(true); });
    connect(list_, &QListWidget::itemDoubleClicked, open_, [this] { open_->click(); });
    connect(mount_button, &QPushButton::clicked, this, [this] {
        const auto path =
            QFileDialog::getExistingDirectory(this, "选择包含 resource_catalog.json 的目录");
        if (!path.isEmpty() && mount(path)) {
            QSettings().setValue("catalog/course_root", path);
        }
    });
    builtin_        = catalog::read(builtin_scene_directory(), true);
    const auto path = QSettings().value("catalog/course_root").toString();
    filter();
    if (!path.isEmpty()) {
        mount(path);
    }
}

bool CatalogBrowser::mount(const QString& directory) {
    try {
        course_ = catalog::read(directory);
        filter();
        notice_->setText(QString("已挂载 %1 个课程条目").arg(course_.size()));
        return true;
    } catch (const std::exception& error) {
        notice_->setText(QString::fromUtf8(error.what()));
        return false;
    }
}

void CatalogBrowser::filter() {
    list_->clear();
    int index = 0;
    for (const auto* entries : {&builtin_, &course_}) {
        for (const auto& entry : *entries) {
            const int current = index++;
            if (type_->currentIndex() &&
                type_->currentIndex() != static_cast<int>(entry.kind) + 1) {
                continue;
            }
            const auto text =
                entry.name + " " + entry.id + " " + entry.source + " " + entry.description;
            if (!text.contains(search_->text().trimmed(), Qt::CaseInsensitive)) {
                continue;
            }
            auto* item = new QListWidgetItem(QIcon(entry.thumbnail), entry.name, list_);
            item->setData(Qt::UserRole, current);
        }
    }
    list_->setCurrentRow(list_->count() ? 0 : -1);
    describe();
}

void CatalogBrowser::describe() {
    auto* item = list_->currentItem();
    open_->setEnabled(item != nullptr);
    recommended_->hide();
    if (!item) {
        detail_->setText("没有匹配的资源");
        open_->setText("打开");
        return;
    }
    int         index = item->data(Qt::UserRole).toInt();
    const auto& e = index < builtin_.size() ? builtin_[index] : course_[index - builtin_.size()];
    const auto  format = e.kind == catalog::Kind::sequence ? "EXR 序列" : "MiniRender JSON";
    recommended_->setVisible(e.kind == catalog::Kind::scene && !e.recommended.isEmpty());
    recommended_->setText(e.recommended == "ssgi"     ? "覆盖并预览屏幕间接光"
                          : e.recommended == "prt"    ? "覆盖并烘焙 PRT"
                          : e.recommended == "energy" ? "覆盖并预览能量补偿"
                          : e.recommended == "pcss"   ? "覆盖并预览 PCSS"
                                                      : "覆盖并预览 Blinn–Phong");
    detail_->setText(QString("%1\n%2 · %3 · %4\n%5\nSHA-256  %6")
                         .arg(e.description, e.source, e.rights, format, e.file.path,
                              QString::fromLatin1(e.file.sha256)));
    open_->setText(e.kind == catalog::Kind::scene   ? "覆盖导入场景"
                   : e.kind == catalog::Kind::model ? "追加模型"
                                                    : "打开帧序列");
}
