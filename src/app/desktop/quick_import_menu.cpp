// 两种导入方式共用同一份目录。模型直接列出，场景模板放在各自的子菜单中。
#include "quick_import_menu.hpp"
#include "builtin_scenes.hpp"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMenu>

namespace {
void fill(QMenu& menu, const QList<catalog::Entry>& entries, catalog::ImportAction mode,
          const std::function<void(const catalog::Entry&, catalog::ImportAction)>& on_import) {
    // 清理上次挂载产生的子菜单，以免其动作仍可通过父菜单找到。
    for (auto* submenu : menu.findChildren<QMenu*>(QString{}, Qt::FindDirectChildrenOnly)) {
        delete submenu;
    }
    menu.clear();
    auto add = [&](QMenu& group, const catalog::Entry& entry) {
        auto*      action = group.addAction(entry.name);
        const auto prefix = mode == catalog::ImportAction::append ? "append_" : "replace_";
        action->setObjectName(entry.builtin && mode == catalog::ImportAction::replace
                                  ? "builtin_" + entry.id
                                  : prefix + entry.id);
        action->setToolTip(entry.description);
        QObject::connect(action, &QAction::triggered, &group,
                         [entry, mode, on_import] { on_import(entry, mode); });
    };
    for (const auto& entry : entries) {
        if (entry.kind == catalog::Kind::model) {
            add(menu, entry);
        }
    }
    menu.addSeparator();
    auto* scenes = menu.addMenu("场景模板");
    for (const auto& entry : entries) {
        if (entry.kind == catalog::Kind::scene) {
            add(*scenes, entry);
        }
    }
}
} // namespace

void add_quick_import_actions(
    QMenu& menu, std::function<void(const catalog::Entry&, catalog::ImportAction)> on_import) {
    auto*                 append  = menu.addMenu("直接导入");
    auto*                 replace = menu.addMenu("覆盖导入");
    QList<catalog::Entry> bundled;
    try {
        bundled = catalog::read(builtin_scene_directory(), true);
    } catch (const std::exception& error) {
        menu.addAction(QString::fromUtf8(error.what()))->setEnabled(false);
    }
    const auto populate = [append, replace, on_import](const QList<catalog::Entry>& entries) {
        fill(*append, entries, catalog::ImportAction::append, on_import);
        fill(*replace, entries, catalog::ImportAction::replace, on_import);
    };
    populate(bundled);
    auto* notice = menu.addAction("课程包不可用，请在资源目录中重新选择");
    notice->setEnabled(false);
    notice->setVisible(false);
    // 首次打开时读取本地课程包。重新挂载或重新生成索引后再更新菜单，无需重启工作台。
    QObject::connect(
        &menu, &QMenu::aboutToShow, &menu,
        [bundled, populate, notice, root = QString{}, modified = QDateTime{}]() mutable {
            const auto next = catalog::course_directory();
            const auto stamp =
                next.isEmpty()
                    ? QDateTime{}
                    : QFileInfo(QDir(next).filePath("resource_catalog.json")).lastModified();
            if (root == next && modified == stamp) {
                return;
            }
            root         = next;
            modified     = stamp;
            auto entries = bundled;
            notice->setVisible(!next.isEmpty() && !stamp.isValid());
            if (stamp.isValid()) {
                try {
                    entries.append(catalog::read(root));
                } catch (const std::exception& error) {
                    notice->setToolTip(QString::fromUtf8(error.what()));
                    notice->setVisible(true);
                }
            }
            populate(entries);
        });
}
