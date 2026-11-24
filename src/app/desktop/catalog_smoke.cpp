// 从快速导入菜单操作真实控件，并在每次后台任务完成后检查编辑器状态。
#include "catalog_smoke.hpp"
#include "builtin_scenes.hpp"
#include "catalog_browser.hpp"
#include "workbench.hpp"
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <minirender/assets/scene_io.hpp>

namespace {
void require(bool ok, const char* message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}

struct State {
    int                       stage  = 0;
    bool                      course = false;
    catalog::Entry            model, sequence;
    qsizetype                 frame_count = 0;
    std::size_t               initial = 0, additions = 0;
    minirender::SceneDocument original, standalone;
};

// 隐藏窗口仍走正式菜单和选择回调。课程包由环境变量提供，默认使用随包测试资源。
void choose(Workbench& window, const QString& text, catalog::ImportAction action) {
    window.findChild<QAction*>("browse_catalog")->trigger();
    auto* browser = dynamic_cast<CatalogBrowser*>(window.findChild<QDialog*>("resource_browser"));
    require(browser, "Browser did not open");
    const auto path = qEnvironmentVariable("MINIRENDER_TEST_COURSE_ROOT");
    if (!path.isEmpty()) {
        require(browser->mount(path), "Course package did not mount");
    }
    browser->findChild<QLineEdit*>()->setText(text);
    auto* list = browser->findChild<QListWidget*>();
    require(list->count() > 0, "Resource search returned no results");
    const bool replace = action == catalog::ImportAction::replace;
    auto*      button =
        browser->findChild<QPushButton*>(replace ? "catalog_replace" : "catalog_append_or_open");
    const QString label = action == catalog::ImportAction::open_sequence ? "打开帧序列"
                          : replace                                      ? "覆盖导入"
                                                                         : "直接导入";
    require(button && !button->isHidden() && button->isEnabled() && button->text() == label,
            "Missing requested catalog action");
    button->click();
}

// 从工具栏的实际子菜单触发导入。测试通过菜单信号填充课程条目，全程保持窗口隐藏。
void quick(Workbench& window, const QString& id) {
    auto* menu = window.findChild<QToolButton*>("builtin_scenes")->menu();
    QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection);
    auto* action = menu->findChild<QAction*>(id);
    require(action && action->isEnabled(), "Quick import menu entry is missing");
    action->trigger();
}

} // namespace

void start_catalog_smoke(Workbench& window, const QString& directory) {
    auto  s     = std::make_shared<State>();
    auto* timer = new QTimer(&window);
    QObject::connect(timer, &QTimer::timeout, &window, [&window, s, timer, directory] {
        try {
            if (!window.viewport()->isValid() || window.asset_loading()) {
                return;
            }
            switch (s->stage) {
            case 0: {
                s->original     = window.document();
                s->initial      = window.document().objects.size();
                const auto path = qEnvironmentVariable("MINIRENDER_TEST_COURSE_ROOT");
                s->course       = !path.isEmpty();
                const auto entries =
                    catalog::read(s->course ? path : builtin_scene_directory(), !s->course);
                for (const auto& e : entries) {
                    if (e.id == (s->course ? "mary" : "spot")) {
                        s->model = e;
                    }
                    if (e.id == (s->course ? "pink-room" : "motion")) {
                        s->sequence = e;
                    }
                }
                s->additions   = s->course ? 2 : 1;
                s->frame_count = s->course ? 80 : 4;
                choose(window, s->model.name, catalog::ImportAction::append);
                break;
            }
            case 1: {
                require(window.document().objects.size() == s->initial + s->additions,
                        "Append lost or duplicated objects");
                const auto& doc = window.document();
                require(doc.camera.position == s->original.camera.position &&
                            doc.camera.target == s->original.camera.target &&
                            doc.environment == s->original.environment &&
                            doc.directional_lights.size() == s->original.directional_lights.size(),
                        "Appending a model changed global settings");
                require(doc.textures.size() == s->original.textures.size() + 1,
                        "Model texture was not imported");
                window.findChild<QAction*>("undo")->trigger();
                require(window.document().objects.size() == s->initial,
                        "Append requires more than one undo");
                window.findChild<QAction*>("redo")->trigger();
                const auto filename = directory + "/目录保存.json";
                window.save_scene_file(filename);
                const auto saved =
                    minirender::load_scene(std::filesystem::path(filename.toStdU16String()));
                require(saved.objects.size() == s->initial + s->additions,
                        "Reopened scene lost appended objects");
                require(saved.meshes.back().package_on_save &&
                            *saved.meshes.back().source.begin() == "assets",
                        "Packaged mesh points outside saved scene");
                {
                    require(!saved.textures.empty() && saved.textures.back().package_on_save &&
                                *saved.textures.back().source.begin() == "assets",
                            "Packaged texture points outside saved scene");
                }
                auto bad = s->model;
                bad.file.sha256.fill('0');
                window.import_resource(bad, catalog::ImportAction::replace);
                break;
            }
            case 2: {
                require(window.document().objects.size() == s->initial + s->additions,
                        "Failed import changed scene");
                auto* error = window.findChild<QMessageBox*>("catalog_error");
                require(error && error->text().contains("校验失败"), "Checksum error missing");
                error->accept();
                window.import_resource(s->model, catalog::ImportAction::replace);
                auto* progress = window.findChild<QProgressDialog*>("catalog_progress");
                require(progress, "Missing cancel control");
                progress->findChild<QPushButton*>()->click();
                break;
            }
            case 3:
                require(window.document().objects.size() == s->initial + s->additions,
                        "Cancelled import changed scene");
                choose(window, s->sequence.name, catalog::ImportAction::open_sequence);
                break;
            case 4:
            case 5: {
                auto* viewer = window.findChild<QDialog*>("sequence_viewer");
                require(viewer, "Sequence viewer missing");
                auto* slider = viewer->findChild<QSlider*>();
                auto* status = viewer->findChild<QLabel*>("frame_status");
                if (!slider->isEnabled() || !status->text().contains(" / ")) {
                    return;
                }
                require(slider->maximum() == s->frame_count - 1, "Incomplete frame index");
                if (s->stage == 4) {
                    slider->setValue(1);
                    slider->setValue(slider->maximum());
                    break;
                }
                require(status->text().startsWith(QString::number(s->frame_count) + " / "),
                        "Seek returned stale frame");
                require(window.document().objects.size() == s->initial + s->additions,
                        "Frame viewer changed the editable document");
                viewer->close();
                choose(window, s->model.name, catalog::ImportAction::replace);
                break;
            }
            case 6: {
                const auto& doc = window.document();
                require(doc.objects.size() == s->additions && doc.meshes.size() == s->additions &&
                            doc.textures.size() == 1,
                        "Standalone import retained old geometry or lost textures");
                const auto expected = minirender::load_scene(std::filesystem::path(
                    catalog::resolve(s->model.root, s->model.file.path).toStdU16String()));
                require(doc.camera.position == expected.camera.position &&
                            doc.camera.target == expected.camera.target &&
                            doc.directional_lights.size() == expected.directional_lights.size(),
                        "Standalone model lost its framing or lights");
                for (std::size_t i = 0; i < doc.objects.size(); ++i) {
                    require(doc.objects[i].name == expected.objects[i].name &&
                                doc.objects[i].material_id == expected.objects[i].material_id,
                            "Standalone model parts changed");
                }
                quick(window, "replace_suzanne");
                break;
            }
            case 7:
                s->standalone = window.document();
                require(s->standalone.objects.size() == 1 &&
                            s->standalone.objects.front().name.find("Suzanne") !=
                                std::string::npos &&
                            s->standalone.meshes.front().data->triangles.size() == 3936,
                        "Standalone Suzanne is missing or retained the previous model");
                quick(window, "append_bounded_fog");
                break;
            case 8: {
                const auto& doc = window.document();
                require(doc.objects.size() == s->standalone.objects.size() + 3 &&
                            doc.media.size() == 1 &&
                            doc.directional_lights.size() ==
                                s->standalone.directional_lights.size() + 1 &&
                            doc.camera.position == s->standalone.camera.position &&
                            doc.environment == s->standalone.environment,
                        "Scene append lost contents or replaced global settings");
                window.findChild<QAction*>("undo")->trigger();
                require(window.document().objects.size() == 1 && window.document().media.empty() &&
                            window.document().directional_lights.size() ==
                                s->standalone.directional_lights.size(),
                        "Scene append did not undo in one step");
                quick(window, "append_spot");
                break;
            }
            case 9:
                require(window.document().objects.size() == 2 &&
                            window.document().meshes.back().data->triangles.size() == 5856 &&
                            window.document().textures.size() == 1,
                        "Quick Spot append lost the cow or its markings");
                quick(window, "builtin_cornell");
                break;
            case 10:
                require(window.document().objects.size() > 2 &&
                            window.document().textures.empty() &&
                            window.document().objects.front().id == "floor",
                        "Quick scene replacement retained prior objects");
                // 从已挂载的课程包或本机安装记录进入 Mary 快捷项，无需再次打开目录。
                if (s->course && QDir(catalog::course_directory()).canonicalPath() ==
                                     QDir(s->model.root).canonicalPath()) {
                    quick(window, "replace_mary");
                    break;
                }
                [[fallthrough]];
            default:
                if (s->stage == 11) {
                    require(window.document().objects.size() == 2 &&
                                window.document().textures.size() == 1,
                            "Quick Mary replacement lost body, accessory or texture");
                    quick(window, "append_mary");
                    break;
                }
                if (s->stage == 12) {
                    require(window.document().objects.size() == 4 &&
                                window.document().textures.size() == 2,
                            "Quick Mary append did not retain both copies");
                }
                if (s->stage == 12) {
                    qInfo() << "Mary toolbar shortcuts verified for" << s->model.root;
                }
                timer->stop();
                qInfo() << "Catalog smoke PASS: direct and standalone imports, model materials, "
                           "scene lights/media, undo, portable assets, cancel and seek";
                QApplication::exit(0);
                return;
            }
            ++s->stage;
        } catch (const std::exception& error) {
            timer->stop();
            qCritical() << "Catalog smoke FAIL:" << error.what();
            QApplication::exit(1);
        }
    });
    timer->start(80);
}
