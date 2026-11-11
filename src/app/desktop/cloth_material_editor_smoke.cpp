#include "cloth_material_editor_smoke.hpp"
#include "cloth_smoke.hpp"
#include "geometry_smoke.hpp"
#include "ui_response_probe.hpp"
#include "workbench.hpp"

#include <minirender/assets/scene_io.hpp>
#include <minirender/render/path_tracer.hpp>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>

#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
template <class T> T* control(Workbench& window, const char* name) {
    auto* result = window.findChild<T*>(name);
    require(result != nullptr, "Cloth material control is missing");
    return result;
}
const minirender::Material& material(Workbench& window) {
    return window.document().materials[0];
}
void number(Workbench& window, const char* name, double value) {
    control<QDoubleSpinBox>(window, name)->setValue(value);
}
void click(Workbench& window, const char* name) {
    control<QPushButton>(window, name)->click();
}
void text(Workbench& window, const char* name, const QString& value) {
    auto* field = control<QLineEdit>(window, name);
    field->setText(value);
    field->setModified(true);
}

// 将嵌套参数映射到物体页，检查命名字段之间的遮挡、水平边界与滚动可达性。
void verify_layout(Workbench& window, const QString& directory, const QString& label) {
    auto* tabs = control<QTabWidget>(window, "inspector_tabs");
    auto* area = qobject_cast<QScrollArea*>(tabs->currentWidget());
    require(area != nullptr, "Object inspector is not scrollable");
    auto* page = area->widget();
    require(page->width() <= area->viewport()->width(), "Cloth editor needs horizontal scrolling");
    std::vector<std::pair<QWidget*, QRect>> fields;
    QWidget* bottom = nullptr;
    int last = 0;
    for (auto* field : page->findChildren<QWidget*>()) {
        if (!field->objectName().startsWith("cloth_") ||
            field->objectName() == "cloth_material_panel" || !field->isVisibleTo(page)) {
            continue;
        }
        const QRect rect(field->mapTo(page, QPoint()), field->size());
        require(rect.left() >= 0 && rect.right() < page->width(),
                "Cloth material field is clipped");
        for (const auto& [other, old] : fields) {
            const auto overlap = rect.intersected(old);
            if (overlap.width() > 1 && overlap.height() > 1) {
                qInfo() << "Cloth field overlap" << field->objectName() << rect
                        << other->objectName() << old;
            }
            require(overlap.width() <= 1 || overlap.height() <= 1, "Cloth material fields overlap");
        }
        fields.emplace_back(field, rect);
        if (rect.bottom() > last) {
            last = rect.bottom();
            bottom = field;
        }
    }
    require(fields.size() >= 25 && bottom, "Cloth material fields are missing");
    area->ensureWidgetVisible(bottom);
    require(
        area->viewport()->rect().contains(bottom->mapTo(area->viewport(), bottom->rect().center())),
        "Cloth material bottom controls are unreachable");
    capture_workbench_ui(window, directory + '/' + label + "-bottom.png");
    area->ensureWidgetVisible(control<QComboBox>(window, "material_kind"));
    capture_workbench_ui(window, directory + '/' + label + "-top.png");
    qInfo() << "Cloth material layout" << window.size() << fields.size();
}

struct State {
    int stage = 0, frame = 0, ticks = 0, edits = 0;
    UiResponseProbe* probe = nullptr;
    std::uint64_t meshes = 0, descriptors = 0;
    std::shared_ptr<const minirender::ClothAppearance> before_curve, before_preset;
    std::shared_ptr<const minirender::SceneSnapshot> frozen_snapshot;
    minirender::Image reference{12, 12};
};

} // namespace

void start_cloth_material_editor_smoke(Workbench& window, const QString& directory,
                                       minirender::SceneDocument scene, bool enforce_response) {
    const auto path = std::filesystem::path((directory + "/布料 材质编辑.json").toStdU16String());
    minirender::save_scene(scene, path);
    window.load_scene_file(QString::fromStdU16String(path.u16string()));
    window.select_object("cloth");
    control<QTabWidget>(window, "inspector_tabs")->setCurrentIndex(0);
    window.resize(1200, 800);
    auto state = std::make_shared<State>();
    state->probe = new UiResponseProbe(&window, "Cloth");
    auto* timer = new QTimer(&window);
    timer->setInterval(70);
    QObject::connect(
        timer, &QTimer::timeout, &window,
        [state, timer, &window, directory, path, enforce_response] {
            auto* viewport = window.viewport();
            try {
                require(++state->ticks < 1500 && viewport->failure().isEmpty(),
                        "Cloth material editor timed out");
                if (viewport->rendered_frames() <= state->frame + 2) {
                    return;
                }
                const auto advance = [&] {
                    ++state->stage;
                    state->frame = viewport->rendered_frames();
                };
                switch (state->stage) {
                case 0:
                    require(control<QComboBox>(window, "material_kind")->currentIndex() == 4,
                            "Cloth material kind is not displayed");
                    require(!control<QDoubleSpinBox>(window, "roughness")->isEnabled(),
                            "Cloth roughness should use thread widths");
                    state->meshes = viewport->upload_statistics().mesh_uploads;
                    state->descriptors = viewport->upload_statistics().descriptor_rebuilds;
                    state->frozen_snapshot = minirender::make_snapshot(window.document());
                    state->probe->verify([&] {
                        state->reference =
                            minirender::PathTracer(state->frozen_snapshot).render({12, 12, 8, 101});
                    });
                    number(window, "cloth_rotation", 23);
                    number(window, "cloth_warp.surface_width", 7.5);
                    number(window, "cloth_warp.color.0", .25);
                    number(window, "cloth_warp_ratio", .7);
                    control<QSpinBox>(window, "cloth_tangent_samples")->setValue(12);
                    require(material(window).cloth->settings().rotation_degrees == 23,
                            "Cloth rotation edit failed");
                    require(material(window).cloth->settings().threads[0].surface_width_degrees ==
                                7.5,
                            "Cloth width edit failed");
                    advance();
                    return;
                case 1:
                    state->probe->verify(
                        [&] { verify_cloth_gpu(window, directory, "cloth-ui-edited"); });
                    require(viewport->upload_statistics().mesh_uploads == state->meshes &&
                                viewport->upload_statistics().descriptor_rebuilds ==
                                    state->descriptors,
                            "Cloth UI edit rebuilt geometry or descriptors");
                    state->before_curve = material(window).cloth;
                    text(window, "cloth_warp.angles", "-15, 0, 25");
                    text(window, "cloth_warp.lengths", "1");
                    click(window, "cloth_warp.apply_curve");
                    require(material(window).cloth == state->before_curve,
                            "Invalid curve changed the material");
                    require(!control<QLabel>(window, "cloth_material_notice")->text().isEmpty(),
                            "Invalid curve has no message");
                    text(window, "cloth_warp.lengths", "1, 2");
                    number(window, "cloth_rotation", 24);
                    require(control<QLineEdit>(window, "cloth_warp.angles")->text() == "-15, 0, 25",
                            "Parameter refresh discarded unapplied curve text");
                    click(window, "cloth_warp.apply_curve");
                    require(material(window).cloth->settings().threads[0].curve.angles ==
                                std::vector<double>{-15, 0, 25},
                            "Curve transaction failed");
                    state->before_preset = material(window).cloth;
                    control<QComboBox>(window, "cloth_preset")->setCurrentIndex(3);
                    click(window, "cloth_apply_preset");
                    require(material(window).cloth->settings().coverage[0] == .86,
                            "Cloth preset was not applied");
                    control<QAction>(window, "undo")->trigger();
                    require(material(window).cloth == state->before_preset,
                            "Cloth preset undo failed");
                    control<QAction>(window, "redo")->trigger();
                    require(material(window).cloth->settings().coverage[0] == .86,
                            "Cloth preset redo failed");
                    advance();
                    return;
                case 2:
                    state->probe->verify([&] {
                        verify_cloth_gpu(window, directory, "cloth-ui-preset");
                        const auto old =
                            minirender::PathTracer(state->frozen_snapshot).render({12, 12, 8, 101});
                        for (std::size_t y = 0; y < 12; ++y) {
                            for (std::size_t x = 0; x < 12; ++x) {
                                require(old.at(x, y) == state->reference.at(x, y),
                                        "Cloth edit modified old offline snapshot");
                            }
                        }
                    });
                    window.save_scene_file(QString::fromStdU16String(path.u16string()));
                    text(window, "cloth_warp.angles", "uncommitted");
                    window.load_scene_file(QString::fromStdU16String(path.u16string()));
                    window.select_object("cloth");
                    require(control<QLineEdit>(window, "cloth_warp.angles")->text() !=
                                "uncommitted",
                            "Scene load kept old staged curve text");
                    require(material(window).cloth->settings().coverage[0] == .86,
                            "Cloth save/reload lost the preset");
                    advance();
                    return;
                case 3:
                    state->probe->verify([&] {
                        verify_cloth_gpu(window, directory, "cloth-ui-reopened");
                        verify_layout(window, directory, "cloth-layout-1200");
                    });
                    window.resize(960, 700);
                    advance();
                    return;
                case 4:
                    state->probe->verify(
                        [&] { verify_layout(window, directory, "cloth-layout-960"); });
                    state->meshes = viewport->upload_statistics().mesh_uploads;
                    state->descriptors = viewport->upload_statistics().descriptor_rebuilds;
                    advance();
                    return;
                case 5:
                    number(window, "cloth_rotation", double(state->edits * 3));
                    number(window, "cloth_warp.surface_width", 5 + double(state->edits % 5));
                    number(window, "cloth_weft.kd", .1 + .05 * double(state->edits % 4));
                    if (++state->edits < 30) {
                        return;
                    }
                    advance();
                    return;
                case 6:
                    require(viewport->upload_statistics().mesh_uploads == state->meshes &&
                                viewport->upload_statistics().descriptor_rebuilds ==
                                    state->descriptors,
                            "Cloth stress edits uploaded static resources");
                    state->probe->report(enforce_response);
                    timer->stop();
                    qInfo() << "Cloth smoke PASS: shading, controls, curves, history, files, "
                               "layout and response";
                    QApplication::exit(0);
                    return;
                }
            } catch (const std::exception& error) {
                timer->stop();
                qCritical() << "Cloth material editor FAIL stage" << state->stage << ':'
                            << error.what();
                QApplication::exit(1);
            }
        });
    timer->start();
}
