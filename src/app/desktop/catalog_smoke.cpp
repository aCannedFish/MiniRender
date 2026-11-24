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
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <minirender/assets/scene_io.hpp>

namespace {
void require(bool ok, const char* message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}

struct State {
    int            stage  = 0;
    bool           course = false;
    catalog::Entry model, sequence;
    qsizetype      frame_count = 0;
    std::size_t    initial = 0, additions = 0;
};

// 隐藏窗口仍走正式菜单和选择回调。课程包由环境变量提供，默认使用随包测试资源。
void choose(Workbench& window, const QString& text) {
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
    for (auto* button : browser->findChildren<QPushButton*>()) {
        if (button->text() == "追加模型" || button->text() == "打开帧序列") {
            button->click();
            return;
        }
    }
    throw std::runtime_error("Incorrect browser action");
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
                s->initial      = window.document().objects.size();
                const auto path = qEnvironmentVariable("MINIRENDER_TEST_COURSE_ROOT");
                s->course       = !path.isEmpty();
                const auto entries =
                    catalog::read(s->course ? path : builtin_scene_directory(), !s->course);
                for (const auto& e : entries) {
                    if (e.id == (s->course ? "mary" : "unit-cube")) {
                        s->model = e;
                    }
                    if (e.id == (s->course ? "pink-room" : "motion")) {
                        s->sequence = e;
                    }
                }
                s->additions   = s->course ? 2 : 1;
                s->frame_count = s->course ? 80 : 4;
                choose(window, s->model.name);
                break;
            }
            case 1: {
                require(window.document().objects.size() == s->initial + s->additions,
                        "Append lost or duplicated objects");
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
                if (s->course) {
                    require(saved.textures.back().package_on_save &&
                                *saved.textures.back().source.begin() == "assets",
                            "Packaged texture points outside saved scene");
                }
                auto bad = s->model;
                bad.file.sha256.fill('0');
                window.import_resource(bad);
                break;
            }
            case 2: {
                require(window.document().objects.size() == s->initial + s->additions,
                        "Failed import changed scene");
                auto* error = window.findChild<QMessageBox*>("catalog_error");
                require(error && error->text().contains("校验失败"), "Checksum error missing");
                error->accept();
                window.import_resource(s->model);
                auto* progress = window.findChild<QProgressDialog*>("catalog_progress");
                require(progress, "Missing cancel control");
                progress->findChild<QPushButton*>()->click();
                break;
            }
            case 3:
                require(window.document().objects.size() == s->initial + s->additions,
                        "Cancelled import changed scene");
                choose(window, s->sequence.name);
                break;
            default: {
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
                timer->stop();
                qInfo() << "Catalog smoke PASS: search, append, undo, portable save, digest, "
                           "cancel and frame seek";
                QApplication::exit(0);
                return;
            }
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
