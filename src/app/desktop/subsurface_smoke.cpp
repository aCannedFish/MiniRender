#include "subsurface_smoke.hpp"
#include "geometry_smoke.hpp"
#include "offline_panel.hpp"
#include "ui_response_probe.hpp"
#include "workbench.hpp"

#include <minirender/assets/scene_io.hpp>
#include <minirender/render/dipole.hpp>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLayout>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabWidget>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace {
using namespace minirender;
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
template <class T> T* control(Workbench& window, const char* name) {
    auto* result = window.findChild<T*>(name);
    require(result, "SSS regression control is missing");
    return result;
}
void click(Workbench& window, const char* name) {
    auto* button = control<QPushButton>(window, name);
    if (!button->isEnabled()) {
        throw std::runtime_error(std::string("SSS button is disabled: ") + name);
    }
    button->click();
}
void action(Workbench& window, const char* name) {
    auto* value = control<QAction>(window, name);
    require(value->isEnabled(), "SSS history/import action is disabled");
    value->trigger();
}
void set(Workbench& window, const char* name, double value) {
    control<QDoubleSpinBox>(window, name)->setValue(value);
}
const Material& material(Workbench& window) {
    const auto& scene = window.document();
    const auto object = std::find_if(scene.objects.begin(), scene.objects.end(),
                                     [](const auto& o) { return o.id == "block"; });
    require(object != scene.objects.end(), "SSS block is missing");
    const auto result = std::find_if(scene.materials.begin(), scene.materials.end(),
                                     [&](const auto& m) { return m.id == object->material_id; });
    require(result != scene.materials.end(), "SSS material is missing");
    return *result;
}
std::filesystem::path native(const QString& path) {
    return std::filesystem::path(path.toStdU16String());
}

// 任务图像与独立串行参考逐像素比较，参数草稿和后续编辑不会参与原快照。
void verify_result(Workbench& window, const std::shared_ptr<const SceneSnapshot>& snapshot,
                   RenderSettings render, IntegratorSettings integrator) {
    const auto expected = PathTracer(snapshot, integrator).render(render);
    const auto actual = window.offline_panel()->session().result();
    require(actual && actual->width() == render.width && actual->height() == render.height,
            "SSS result dimensions differ");
    double maximum = 0;
    for (std::size_t y = 0; y < render.height; ++y) {
        for (std::size_t x = 0; x < render.width; ++x) {
            maximum = std::max(maximum, double(glm::length(actual->at(x, y) - expected.at(x, y))));
        }
    }
    const auto p = window.offline_panel()->session().progress();
    require(maximum < 1e-6 && p.statistics.subsurface_events > 0 &&
                p.integrator == IntegratorKind::path_tracing,
            "SSS task differs from its CPU reference or selected backend");
    qInfo() << "Subsurface serial RGB maximum" << maximum << "events"
            << p.statistics.subsurface_events;
}
void verify_display(Workbench& window, const QString& directory, const QString& label) {
    const auto actual = window.viewport()->grab();
    const auto source = window.offline_panel()->session().result();
    require(source && !actual.isNull() && window.viewport()->offline_visible(),
            "SSS display is missing");
    const auto expected = display_image(*source, window.viewport()->exposure());
    const double scale = std::min(double(actual.width()) / double(source->width()),
                                  double(actual.height()) / double(source->height()));
    const int width = int(std::floor(double(source->width()) * scale)),
              height = int(std::floor(double(source->height()) * scale));
    const int left = (actual.width() - width) / 2, top = (actual.height() - height) / 2;
    for (std::size_t y = 0; y < source->height(); ++y) {
        for (std::size_t x = 0; x < source->width(); ++x) {
            const auto pixel =
                actual.pixelColor(left + int((double(x) + .5) * width / double(source->width())),
                                  top + int((double(y) + .5) * height / double(source->height())));
            const std::array channels{pixel.red(), pixel.green(), pixel.blue()};
            for (int c = 0; c < 3; ++c) {
                const auto wanted = encode_unorm8(linear_to_srgb(expected.at(x, y)[c]));
                require(std::abs(channels[std::size_t(c)] - wanted) <= 2,
                        "SSS display differs from CPU encoding");
            }
        }
    }
    require(actual.save(directory + '/' + label + ".png"), "Cannot save SSS display");
    qInfo() << "Subsurface GPU display" << label << source->width() * source->height();
}
void verify_layout(Workbench& window, const QString& directory) {
    auto* tabs = control<QTabWidget>(window, "inspector_tabs");
    tabs->setCurrentIndex(0);
    auto* area = qobject_cast<QScrollArea*>(tabs->currentWidget());
    auto* panel = control<QWidget>(window, "subsurface_material_panel");
    require(area, "SSS page is not scrollable");
    for (const auto size : {QSize(1200, 800), QSize(960, 700)}) {
        window.resize(size);
        window.layout()->activate();
        if (auto* layout = area->layout()) {
            layout->activate();
        }
        panel->layout()->activate();
        require(area->widget()->width() <= area->viewport()->width(),
                "SSS page requires horizontal scrolling");
        std::vector<QRect> rectangles;
        for (auto* field : panel->findChildren<QWidget*>()) {
            if (!field->objectName().startsWith("subsurface_") || !field->isVisibleTo(panel) ||
                field->height() == 0) {
                continue;
            }
            const QRect rect(field->mapTo(panel, QPoint()), field->size());
            require(rect.left() >= 0 && rect.right() < panel->width(),
                    "SSS field exceeds panel width");
            for (const auto& other : rectangles) {
                const auto overlap = rect.intersected(other);
                require(overlap.width() <= 1 || overlap.height() <= 1, "SSS controls overlap");
            }
            rectangles.push_back(rect);
        }
        require(rectangles.size() >= 14, "SSS layout omitted controls");
        area->ensureWidgetVisible(control<QLabel>(window, "subsurface_hint"));
        capture_workbench_ui(window, directory + QString("/layout-%1.png").arg(size.width()));
        auto* bottom = control<QPushButton>(window, "subsurface_render");
        area->ensureWidgetVisible(bottom);
        require(area->viewport()->rect().contains(
                    bottom->mapTo(area->viewport(), bottom->rect().center())),
                "SSS render button is inaccessible");
        capture_workbench_ui(window, directory + QString("/bottom-%1.png").arg(size.width()));
        qInfo() << "Subsurface layout" << size << rectangles.size();
    }
}
enum class Stage {
    import,
    ready,
    draft,
    invalid,
    history,
    preset,
    texture_unassigned,
    texture_ready,
    preview,
    preview_reference,
    preview_back,
    old_running,
    old_done,
    old_verify,
    new_done,
    new_verify,
    exposure,
    exported,
    stress,
    bad_ready,
    bad_failed,
    cancel_ready,
    cancelling,
    cancelled,
    exit_running
};
struct State {
    Stage stage = Stage::import;
    int ticks = 0, frame = 0, settle = 0, edits = 0;
    SubsurfaceParameters precise;
    std::shared_ptr<const SceneSnapshot> snapshot;
    RenderSettings render{24, 24, 1024, 101};
    IntegratorSettings integrator{SamplingMode::mis, 6, true, 3};
    std::optional<Image> realtime;
    SceneUploadStatistics uploads;
    std::uint64_t job = 0, samples = 0;
    QElapsedTimer cancellation;
    UiResponseProbe* probe = nullptr;
};
} // namespace

void start_subsurface_smoke(Workbench& window, const QString& directory, bool enforce_response) {
    require(QDir().mkpath(directory), "Cannot create SSS regression directory");
    auto state = std::make_shared<State>();
    state->probe = new UiResponseProbe(&window, "Subsurface");
    auto* timer = new QTimer(&window);
    timer->setInterval(50);
    QObject::connect(
        timer, &QTimer::timeout, &window, [state, timer, &window, directory, enforce_response] {
            try {
                auto* viewport = window.viewport();
                auto* offline = window.offline_panel();
                const auto p = offline->session().progress();
                require(++state->ticks < 5000 && viewport->failure().isEmpty(),
                        "SSS smoke timed out");
                if (!viewport->can_render() || viewport->rendered_frames() <= state->frame + 2) {
                    return;
                }
                const auto next = [&] {
                    state->stage = static_cast<Stage>(int(state->stage) + 1);
                    state->frame = viewport->rendered_frames();
                    state->settle = 0;
                    qInfo() << "Subsurface stage" << int(state->stage);
                };
                const auto completed_image = [&] {
                    require(p.state != RenderState::failed, p.error.c_str());
                    return p.state == RenderState::completed &&
                           control<QPushButton>(window, "offline_start")->isEnabled() &&
                           viewport->offline_image() == offline->session().result() &&
                           ++state->settle >= 3;
                };
                switch (state->stage) {
                case Stage::import:
                    action(window, "builtin_sss_wax_edge");
                    require(window.asset_loading(), "SSS quick import did not run asynchronously");
                    next();
                    return;
                case Stage::ready: {
                    if (window.asset_loading() || window.document().objects.size() != 2) {
                        return;
                    }
                    window.select_object("block");
                    require(material(window).kind == MaterialKind::subsurface &&
                                !control<QDoubleSpinBox>(window, "roughness")->isEnabled(),
                            "SSS controls or material selection are missing");
                    auto scene = window.document();
                    auto precise = *material(window).subsurface;
                    precise.absorption.x = .12345678901234567;
                    precise.reduced_scattering.y = 612.3456789012345;
                    precise.density = .7123456789012345;
                    state->precise = precise;
                    for (auto& m : scene.materials) {
                        if (m.kind == MaterialKind::subsurface) {
                            m.subsurface = std::make_shared<const SubsurfaceParameters>(precise);
                        }
                    }
                    save_scene(scene, native(directory + "/精度.json"));
                    window.load_scene_file(directory + "/精度.json");
                    window.select_object("block");
                    set(window, "subsurface_absorption.0", 4.5);
                    set(window, "light_intensity",
                        window.document().directional_lights[0].intensity + 1);
                    next();
                    return;
                }
                case Stage::draft:
                    require(
                        *material(window).subsurface == state->precise &&
                            control<QDoubleSpinBox>(window, "subsurface_absorption.0")->value() ==
                                4.5 &&
                            !control<QPushButton>(window, "subsurface_render")->isEnabled(),
                        "Unrelated edit lost the SSS draft or rendered unapplied values");
                    window.load_scene_file(directory + "/精度.json");
                    window.select_object("block");
                    require(control<QDoubleSpinBox>(window, "subsurface_absorption.0")->value() ==
                                state->precise.absorption.x,
                            "New document retained an old SSS draft with equal source parameters");
                    set(window, "subsurface_absorption.0", 0);
                    set(window, "subsurface_scattering.0", 0);
                    click(window, "subsurface_apply");
                    require(*material(window).subsurface == state->precise &&
                                !control<QLabel>(window, "subsurface_notice")->text().isEmpty(),
                            "Invalid SSS draft changed the document without a diagnostic");
                    next();
                    return;
                case Stage::invalid:
                    click(window, "subsurface_restore");
                    require(control<QDoubleSpinBox>(window, "subsurface_absorption.0")->value() ==
                                state->precise.absorption.x,
                            "Restore rounded the optical source");
                    set(window, "subsurface_absorption.0", 4.5);
                    click(window, "subsurface_apply");
                    require(material(window).subsurface->absorption.x == 4.5 &&
                                material(window).subsurface->density == state->precise.density &&
                                material(window).subsurface->reduced_scattering.y ==
                                    state->precise.reduced_scattering.y,
                            "SSS apply lost untouched double precision");
                    action(window, "undo");
                    require(*material(window).subsurface == state->precise,
                            "SSS apply was not one history transaction");
                    action(window, "redo");
                    next();
                    return;
                case Stage::history:
                    require(material(window).subsurface->absorption.x == 4.5,
                            "SSS redo lost the edit");
                    control<QComboBox>(window, "subsurface_preset")->setCurrentIndex(1);
                    click(window, "subsurface_apply_preset");
                    require(material(window).ior == 1.44F &&
                                material(window).subsurface->density == 1,
                            "Wax preset did not apply parameters and IOR");
                    control<QComboBox>(window, "material_kind")->setCurrentIndex(0);
                    control<QComboBox>(window, "material_kind")->setCurrentIndex(5);
                    require(material(window).subsurface->reduced_scattering.x == 450,
                            "Material switching lost the SSS parameters");
                    next();
                    return;
                case Stage::preset:
                    state->probe->verify([&] {
                        Image image(2, 2);
                        for (std::size_t y = 0; y < 2; ++y) {
                            for (std::size_t x = 0; x < 2; ++x) {
                                image.at(x, y) = glm::vec3(.5F);
                            }
                        }
                        image.write_png(native(directory + "/纹理.png"));
                    });
                    window.import_texture_file(directory + "/纹理.png", ColorEncoding::srgb);
                    next();
                    return;
                case Stage::texture_unassigned:
                    if (window.texture_loading()) {
                        return;
                    }
                    require(material(window).kind == MaterialKind::subsurface &&
                                material(window).base_color_texture.empty() &&
                                window.document().textures.size() == 1 &&
                                control<QLabel>(window, "material_notice")->text().contains("资源"),
                            "Texture import assigned an unsupported SSS texture or lost the asset");
                    control<QComboBox>(window, "material_kind")->setCurrentIndex(0);
                    window.import_texture_file(directory + "/纹理.png", ColorEncoding::srgb);
                    next();
                    return;
                case Stage::texture_ready:
                    if (window.texture_loading()) {
                        return;
                    }
                    require(!material(window).base_color_texture.empty(),
                            "Texture fixture was not assigned");
                    control<QComboBox>(window, "material_kind")->setCurrentIndex(5);
                    require(material(window).kind == MaterialKind::diffuse &&
                                !material(window).base_color_texture.empty() &&
                                control<QLabel>(window, "material_notice")->text().contains("纹理"),
                            "SSS conversion silently removed a texture");
                    control<QComboBox>(window, "base_texture")->setCurrentIndex(0);
                    control<QComboBox>(window, "material_kind")->setCurrentIndex(5);
                    control<QComboBox>(window, "view_mode")->setCurrentIndex(0);
                    control<QCheckBox>(window, "fxaa_enabled")->setChecked(false);
                    control<QCheckBox>(window, "shadows_enabled")->setChecked(false);
                    next();
                    return;
                case Stage::preview: {
                    if (++state->settle < 3) {
                        return;
                    }
                    state->probe->verify([&] { state->realtime = viewport->grab_hdr(); });
                    window.save_scene_file(directory + "/实时原场景.json");
                    auto reference = window.document();
                    for (auto& m : reference.materials) {
                        if (m.kind == MaterialKind::subsurface) {
                            const DipoleProfile profile(*m.subsurface, m.ior);
                            m.kind = MaterialKind::diffuse;
                            m.base_color =
                                glm::vec3(profile.reflectance() * profile.external_transmission());
                            m.roughness = .35F;
                        }
                    }
                    save_scene(reference, native(directory + "/实时参考.json"));
                    window.load_scene_file(directory + "/实时参考.json");
                    next();
                    return;
                }
                case Stage::preview_reference: {
                    if (++state->settle < 3) {
                        return;
                    }
                    state->probe->verify([&] {
                        const auto reference = viewport->grab_hdr();
                        require(reference.width() == state->realtime->width() &&
                                    reference.height() == state->realtime->height(),
                                "SSS preview reference dimensions differ");
                        double maximum = 0;
                        for (std::size_t y = 0; y < reference.height(); ++y) {
                            for (std::size_t x = 0; x < reference.width(); ++x) {
                                maximum = std::max(maximum,
                                                   double(glm::length(reference.at(x, y) -
                                                                      state->realtime->at(x, y))));
                            }
                        }
                        require(maximum < 1e-5,
                                "SSS GPU approximation differs from explicit PBR reference");
                        qInfo() << "Subsurface preview PBR HDR maximum" << maximum;
                    });
                    window.load_scene_file(directory + "/实时原场景.json");
                    window.select_object("block");
                    next();
                    return;
                }
                case Stage::preview_back:
                    state->probe->verify([&] { verify_layout(window, directory); });
                    state->snapshot = make_snapshot(window.document());
                    offline->set_settings(state->render, state->integrator, {.threads = 2});
                    control<QComboBox>(window, "offline_backend")->setCurrentIndex(1);
                    click(window, "subsurface_render");
                    state->job = offline->session().progress().job_id;
                    next();
                    return;
                case Stage::old_running:
                    require(p.state != RenderState::failed, p.error.c_str());
                    if (!p.recorded_samples) {
                        return;
                    }
                    require(!render_terminal(p.state),
                            "SSS snapshot fixture completed before the edit");
                    set(window, "subsurface_density", 1.2);
                    click(window, "subsurface_apply");
                    require(offline->stale() && p.job_id == state->job,
                            "SSS edit restarted the frozen task");
                    control<QTabWidget>(window, "inspector_tabs")->setCurrentIndex(0);
                    click(window, "subsurface_render");
                    require(offline->session().progress().job_id == state->job,
                            "View task restarted SSS rendering");
                    state->probe->verify([&] {
                        require(offline->session().wait_for(std::chrono::seconds(10)),
                                "SSS task did not finish for the view-intent check");
                        click(window, "subsurface_render");
                        require(offline->session().progress().job_id == state->job,
                                "View button started a new task after the old task completed");
                    });
                    next();
                    return;
                case Stage::old_done:
                    if (!completed_image()) {
                        return;
                    }
                    next();
                    return;
                case Stage::old_verify:
                    if (control<QPushButton>(window, "subsurface_render")
                            ->text()
                            .contains("查看")) {
                        return;
                    }
                    state->probe->verify([&] {
                        verify_result(window, state->snapshot, state->render, state->integrator);
                        verify_display(window, directory, "old-snapshot");
                    });
                    state->render = {16, 16, 64, 101};
                    state->snapshot = make_snapshot(window.document());
                    offline->set_settings(state->render, state->integrator, {.threads = 2});
                    click(window, "subsurface_render");
                    next();
                    return;
                case Stage::new_done:
                    if (!completed_image()) {
                        return;
                    }
                    next();
                    return;
                case Stage::new_verify:
                    state->probe->verify([&] {
                        verify_result(window, state->snapshot, state->render, state->integrator);
                        verify_display(window, directory, "new-snapshot");
                    });
                    state->job = p.job_id;
                    state->samples = p.recorded_samples;
                    set(window, "offline_exposure", 2);
                    next();
                    return;
                case Stage::exposure:
                    require(p.job_id == state->job && p.recorded_samples == state->samples &&
                                !offline->stale(),
                            "SSS exposure restarted sampling");
                    state->probe->verify([&] { verify_display(window, directory, "exposure"); });
                    offline->save_image(directory + "/result.exr", true);
                    next();
                    return;
                case Stage::exported:
                    if (offline->exporting()) {
                        return;
                    }
                    require(Image::read_exr(native(directory + "/result.exr")).at(8, 8) ==
                                offline->session().result()->at(8, 8),
                            "SSS EXR applied exposure");
                    control<QCheckBox>(window, "offline_show")->setChecked(false);
                    set(window, "offline_exposure", 0);
                    state->uploads = viewport->upload_statistics();
                    control<QTabWidget>(window, "inspector_tabs")->setCurrentIndex(0);
                    next();
                    return;
                case Stage::stress:
                    set(window, "subsurface_absorption.0", state->edits % 2 == 0 ? 20 : 25);
                    set(window, "subsurface_density", state->edits % 2 == 0 ? 1.2 : 1.3);
                    click(window, "subsurface_apply");
                    if (++state->edits < 12) {
                        return;
                    }
                    next();
                    return;
                case Stage::bad_ready: {
                    require(viewport->upload_statistics().mesh_uploads ==
                                    state->uploads.mesh_uploads &&
                                viewport->upload_statistics().descriptor_rebuilds ==
                                    state->uploads.descriptor_rebuilds,
                            "SSS parameter edits rebuilt mesh or descriptor data");
                    window.save_scene_file(directory + "/有效场景.json");
                    auto bad = window.document();
                    for (auto& object : bad.objects) {
                        if (object.id == "block") {
                            object.mesh_id = "plane";
                        }
                    }
                    save_scene(bad, native(directory + "/开放场景.json"));
                    window.load_scene_file(directory + "/开放场景.json");
                    window.select_object("block");
                    offline->set_settings({4, 4, 1, 101}, state->integrator, {.threads = 2});
                    click(window, "subsurface_render");
                    next();
                    return;
                }
                case Stage::bad_failed:
                    if (!render_terminal(p.state) ||
                        !control<QPushButton>(window, "offline_start")->isEnabled()) {
                        return;
                    }
                    require(
                        p.state == RenderState::failed &&
                            control<QLabel>(window, "offline_notice")->text().contains("closed"),
                        "Open SSS mesh lacks an inline render diagnostic");
                    window.load_scene_file(directory + "/有效场景.json");
                    window.select_object("block");
                    next();
                    return;
                case Stage::cancel_ready:
                    offline->set_settings({128, 128, 1000000, 101}, state->integrator,
                                          {.threads = 2});
                    click(window, "subsurface_render");
                    next();
                    return;
                case Stage::cancelling:
                    require(p.state != RenderState::failed, p.error.c_str());
                    if (!p.recorded_samples) {
                        return;
                    }
                    state->cancellation.start();
                    click(window, "offline_cancel");
                    next();
                    return;
                case Stage::cancelled:
                    if (!render_terminal(p.state) ||
                        !control<QPushButton>(window, "offline_start")->isEnabled()) {
                        return;
                    }
                    if (control<QPushButton>(window, "subsurface_render")
                            ->text()
                            .contains("查看")) {
                        return;
                    }
                    require(p.state == RenderState::cancelled &&
                                state->cancellation.elapsed() < 2000,
                            "SSS cancellation blocked");
                    qInfo() << "Subsurface cancellation ms" << state->cancellation.elapsed();
                    click(window, "subsurface_render");
                    next();
                    return;
                case Stage::exit_running:
                    if (p.state != RenderState::running || !p.recorded_samples) {
                        return;
                    }
                    state->probe->report(enforce_response);
                    timer->stop();
                    qInfo() << "Subsurface smoke PASS: drafts, precision, presets, texture "
                               "constraints, PBR preview, frozen tasks, CPU/GPU display, layouts, "
                               "response and shutdown";
                    QApplication::exit(0);
                    return;
                }
            } catch (const std::exception& error) {
                timer->stop();
                qCritical() << "Subsurface smoke FAIL:" << error.what();
                QApplication::exit(1);
            }
        });
    timer->start();
}
