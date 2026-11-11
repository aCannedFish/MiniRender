#include "sdf_editor_smoke.hpp"
#include "geometry_smoke.hpp"
#include "sdf_panel.hpp"
#include "sdf_smoke.hpp"
#include "ui_response_probe.hpp"
#include "workbench.hpp"

#include <minirender/assets/sdf_io.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QTimer>

#include <fstream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
template <class T> T* control(Workbench& window, const char* name) {
    auto* result = window.findChild<T*>(name);
    require(result != nullptr, "SDF control is missing");
    return result;
}
void click(Workbench& window, const char* name) {
    control<QPushButton>(window, name)->click();
}
void number(Workbench& window, const char* name, double value) {
    control<QDoubleSpinBox>(window, name)->setValue(value);
}
void select(Workbench& window, const char* name, int index) {
    control<QComboBox>(window, name)->setCurrentIndex(index);
}
std::string bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

// 独立发送完整鼠标按下/移动/释放，检查每次手势都能重新获得相机控制。
void drag(VulkanViewport& viewport) {
    const auto send = [&](QEvent::Type type, QPointF point, Qt::MouseButton button,
                          Qt::MouseButtons buttons) {
        QMouseEvent event(type, point, viewport.mapToGlobal(point), button, buttons,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(&viewport, &event);
    };
    const QPointF start(viewport.width() * .5, viewport.height() * .5);
    send(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    send(QEvent::MouseMove, start + QPointF(8, 4), Qt::NoButton, Qt::LeftButton);
    send(QEvent::MouseMove, start + QPointF(16, 8), Qt::NoButton, Qt::LeftButton);
    send(QEvent::MouseButtonRelease, start + QPointF(16, 8), Qt::LeftButton, Qt::NoButton);
}

// 控件映射到页面坐标后检查边界和交叠，覆盖嵌套表单中的三分量控件。
void layout(Workbench& window) {
    auto* panel = window.sdf_panel();
    auto* area =
        qobject_cast<QScrollArea*>(control<QTabWidget>(window, "inspector_tabs")->currentWidget());
    require(area && area->widget() == panel, "SDF page is not selected");
    require(panel->width() <= area->viewport()->width(), "SDF panel needs horizontal scrolling");
    std::vector<std::pair<QWidget*, QRect>> fields;
    QWidget* bottom = nullptr;
    int last_y = 0;
    for (auto* child : panel->findChildren<QWidget*>()) {
        if (!child->objectName().startsWith("sdf_") || !child->isVisibleTo(panel)) {
            continue;
        }
        const QRect rect(child->mapTo(panel, QPoint()), child->size());
        require(rect.left() >= 0 && rect.right() < panel->width(),
                "SDF field is clipped horizontally");
        for (const auto& [other, other_rect] : fields) {
            const auto overlap = rect.intersected(other_rect);
            if (overlap.width() > 1 && overlap.height() > 1) {
                qInfo() << "SDF layout overlap" << child->objectName() << rect
                        << other->objectName() << other_rect;
            }
            require(overlap.width() <= 1 || overlap.height() <= 1, "SDF controls overlap");
        }
        fields.emplace_back(child, rect);
        if (rect.bottom() > last_y) {
            last_y = rect.bottom();
            bottom = child;
        }
    }
    require(fields.size() > 35 && bottom, "SDF layout has too few visible fields");
    area->ensureWidgetVisible(bottom);
    require(
        area->viewport()->rect().contains(bottom->mapTo(area->viewport(), bottom->rect().center())),
        "SDF bottom controls cannot be reached");
    qInfo() << "SDF layout" << window.size() << "fields" << fields.size();
}

struct State {
    int stage = 0;
    int frame = 0;
    int ticks = 0;
    int edits = 0;
    minirender::Camera original_camera;
    minirender::Camera saved_camera;
    std::size_t objects = 0;
    std::size_t meshes = 0;
    std::uint64_t mesh_uploads = 0;
    UiResponseProbe* probe = nullptr; // Qt 父对象拥有探针，State 只保留观察指针。
};

} // namespace

void start_sdf_editor_smoke(Workbench& window, const QString& directory, bool enforce_response) {
    auto state = std::make_shared<State>();
    state->original_camera = window.document().camera;
    state->objects = window.document().objects.size();
    state->meshes = window.document().meshes.size();
    state->mesh_uploads = window.viewport()->upload_statistics().mesh_uploads;
    state->probe = new UiResponseProbe(&window, "SDF");
    auto* timer = new QTimer(&window);
    timer->setInterval(70);
    QObject::connect(
        timer, &QTimer::timeout, &window, [state, timer, &window, directory, enforce_response] {
            auto* viewport = window.viewport();
            auto* panel = window.sdf_panel();
            const auto path =
                std::filesystem::path((directory + "/距离场 编辑.sdf.json").toStdU16String());
            const auto file = QString::fromStdU16String(path.u16string());
            try {
                require(++state->ticks < 2000 && viewport->failure().isEmpty(),
                        "SDF editor regression timed out");
                if (viewport->rendered_frames() <= state->frame + 2) {
                    return;
                }
                const auto advance = [&] {
                    ++state->stage;
                    state->frame = viewport->rendered_frames();
                };
                switch (state->stage) {
                case 0:
                    control<QTabWidget>(window, "inspector_tabs")->setCurrentIndex(8);
                    window.resize(1200, 800);
                    click(window, "sdf_example");
                    viewport->set_sdf_view(minirender::SdfView::surface);
                    select(window, "sdf_nodes", 0);
                    number(window, "sdf_position.x", -.6);
                    number(window, "sdf_rotation.z", 15);
                    number(window, "sdf_scale", 1.1);
                    number(window, "sdf_color.x", .65);
                    require(panel->document().nodes[0].translation.x == -.6F,
                            "SDF position edit failed");
                    require(panel->document().nodes[0].rotation_degrees.z == 15,
                            "SDF rotation edit failed");
                    require(panel->document().nodes[0].scale == 1.1F, "SDF scale edit failed");
                    advance();
                    return;
                case 1: {
                    const auto before = panel->document().nodes[0].parameters;
                    number(window, "sdf_parameters.x", 0);
                    require(panel->document().nodes[0].parameters == before,
                            "Invalid radius modified the document");
                    require(!control<QLabel>(window, "sdf_notice")->text().isEmpty(),
                            "Invalid radius has no message");
                    select(window, "sdf_root", 0);
                    click(window, "sdf_add_box");
                    require(panel->document().nodes.size() == 8 && panel->document().root == 0,
                            "Adding a primitive changed the root");
                    number(window, "sdf_position.x", -.2);
                    number(window, "sdf_position.y", 1.25);
                    number(window, "sdf_rotation.y", 20);
                    click(window, "sdf_difference");
                    const auto& doc = panel->document();
                    require(doc.root == 8 && doc.nodes[8].left == 0 && doc.nodes[8].right == 7,
                            "SDF difference did not combine root and selection");
                    advance();
                    return;
                }
                case 2:
                    select(window, "sdf_left", 8);
                    require(panel->document().nodes[8].left == 0, "Cyclic reference was committed");
                    require(control<QComboBox>(window, "sdf_left")->currentIndex() == 0,
                            "Invalid reference was not restored");
                    click(window, "sdf_undo");
                    require(panel->document().nodes.size() == 8 && panel->document().root == 0,
                            "SDF undo did not restore graph");
                    click(window, "sdf_redo");
                    require(panel->document().nodes.size() == 9 && panel->document().root == 8,
                            "SDF redo did not restore graph");
                    click(window, "sdf_remove");
                    require(panel->document().nodes.size() == 9, "Referenced root was removed");
                    select(window, "sdf_root", 0);
                    select(window, "sdf_nodes", 8);
                    click(window, "sdf_remove");
                    require(panel->document().nodes.size() == 8,
                            "Unused operation was not removed");
                    click(window, "sdf_undo");
                    select(window, "sdf_root", 8);
                    advance();
                    return;
                case 3: {
                    auto precise = panel->document();
                    precise.nodes[0].translation.y = .8543217F;
                    precise.trace.epsilon = .000123456789;
                    minirender::save_sdf(precise, path);
                    panel->load_file(file);
                    select(window, "sdf_nodes", 0);
                    number(window, "sdf_position.x", -.59);
                    number(window, "sdf_safety", .85);
                    require(panel->document().nodes[0].translation.y ==
                                precise.nodes[0].translation.y,
                            "Editing X rounded an unchanged Y coordinate");
                    require(panel->document().trace.epsilon == precise.trace.epsilon,
                            "Editing safety rounded an unchanged tolerance");
                }
                    for (int i = 0; i < 6; ++i) {
                        const auto before = viewport->camera().position;
                        drag(*viewport);
                        require(glm::length(viewport->camera().position - before) > .01F,
                                "SDF camera drag stopped responding");
                    }
                    state->saved_camera = viewport->camera();
                    panel->save_file(file);
                    {
                        const auto reopened = minirender::load_sdf(path);
                        require(reopened.camera.position == state->saved_camera.position &&
                                    reopened.camera.target == state->saved_camera.target,
                                "SDF file lost the current camera");
                        require(reopened.nodes.size() == 9 && reopened.root == 8,
                                "SDF file lost graph edits");
                    }
                    click(window, "sdf_example");
                    panel->load_file(file);
                    require(viewport->camera().position == state->saved_camera.position,
                            "SDF load did not restore saved camera");
                    advance();
                    return;
                case 4: {
                    state->probe->verify([&] { verify_sdf_gpu(window, directory, "sdf-edited"); });
                    const auto protected_path = path.parent_path() / "triangle-scene.json";
                    {
                        std::ofstream output(protected_path);
                        output << "{\"format\":\"triangle-scene\"}";
                    }
                    const auto original = bytes(protected_path);
                    bool rejected = false;
                    try {
                        panel->save_file(QString::fromStdU16String(protected_path.u16string()));
                    } catch (const std::exception&) {
                        rejected = true;
                    }
                    require(rejected && bytes(protected_path) == original,
                            "SDF save overwrote another document type");
                    rejected = false;
                    try {
                        panel->load_file(QString::fromStdU16String(protected_path.u16string()));
                    } catch (const std::exception&) {
                        rejected = true;
                    }
                    require(rejected && panel->document().root == 8,
                            "Failed SDF load modified current document");
                    minirender::PointCloud points;
                    points.positions = {{0, 0, 0}, {1, 0, 0}};
                    viewport->set_point_preview_data(minirender::prepare_point_preview(points));
                    viewport->set_point_preview_visible(true);
                    require(!viewport->sdf_visible() &&
                                !control<QCheckBox>(window, "sdf_visible")->isChecked(),
                            "Point view did not close SDF view");
                    viewport->set_sdf_visible(true);
                    require(!viewport->point_preview_visible() &&
                                viewport->camera().position == state->saved_camera.position,
                            "SDF view did not restore camera");
                    viewport->set_offline_visible(true);
                    require(!viewport->sdf_visible() &&
                                viewport->camera().position == state->original_camera.position,
                            "Offline view lost scene camera");
                    viewport->set_sdf_visible(true);
                    viewport->set_sdf_view(minirender::SdfView::normals);
                    select(window, "sdf_nodes", 0);
                    advance();
                    return;
                }
                case 5:
                    state->probe->verify([&] {
                        layout(window);
                        capture_workbench_ui(window, directory + "/sdf-layout-1200-bottom.png");
                    });
                    window.resize(960, 700);
                    advance();
                    return;
                case 6:
                    state->probe->verify([&] {
                        layout(window);
                        capture_workbench_ui(window, directory + "/sdf-layout-960-bottom.png");
                        auto* tabs = control<QTabWidget>(window, "inspector_tabs");
                        auto* area = qobject_cast<QScrollArea*>(tabs->currentWidget());
                        area->ensureWidgetVisible(control<QCheckBox>(window, "sdf_visible"));
                        capture_workbench_ui(window, directory + "/sdf-layout-960-top.png");
                    });
                    advance();
                    return;
                case 7:
                    number(window, "sdf_position.x", -.6 + double(state->edits % 7) * .02);
                    number(window, "sdf_light_intensity", 2 + double(state->edits % 5) * .1);
                    select(window, "sdf_view", state->edits % 4);
                    control<QCheckBox>(window, "sdf_shadows")->setChecked(state->edits % 2 != 0);
                    if (++state->edits < 30) {
                        return;
                    }
                    advance();
                    return;
                case 8:
                    require(window.document().objects.size() == state->objects &&
                                window.document().meshes.size() == state->meshes,
                            "SDF editing modified triangle scene assets");
                    require(window.document().camera.position == state->original_camera.position,
                            "SDF navigation modified scene camera");
                    require(viewport->upload_statistics().mesh_uploads == state->mesh_uploads,
                            "SDF edits uploaded triangle meshes");
                    state->probe->report(enforce_response);
                    timer->stop();
                    qInfo() << "SDF smoke PASS: controls, CSG, history, files, six drags, layout "
                               "and response";
                    QApplication::exit(0);
                    return;
                }
            } catch (const std::exception& error) {
                timer->stop();
                qCritical() << "SDF editor FAIL stage" << state->stage << ':' << error.what();
                QApplication::exit(1);
            }
        });
    timer->start();
}
