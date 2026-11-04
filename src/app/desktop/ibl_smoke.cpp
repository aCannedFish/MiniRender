#include "ibl_smoke.hpp"
#include "ibl_controller.hpp"
#include "workbench.hpp"
#include <minirender/assets/scene_io.hpp>
#include <minirender/render/path_tracer.hpp>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
std::filesystem::path path(const QString& value) {
    const auto bytes = value.toUtf8();
    return std::filesystem::path(std::u8string(bytes.begin(), bytes.end()));
}
void prepare(Workbench& window, const QString& directory) {
    auto scene = window.document();
    scene.camera.position = {3, 2.3F, 4.2F};
    scene.camera.target = {0, 0.4F, 0};
    scene.camera.aperture_radius = 0;
    scene.environment = {0.1F, 0.2F, 0.3F};
    scene.directional_lights.clear();
    scene.materials.clear();
    scene.objects.clear();
    scene.textures.clear();
    scene.materials.push_back({"floor", "floor"});
    scene.materials[0].base_color = {0.5F, 0.6F, 0.7F};
    scene.materials.push_back({"metal", "metal"});
    scene.materials[1].base_color = {0.8F, 0.3F, 0.1F};
    scene.materials[1].kind = minirender::MaterialKind::conductor;
    scene.materials[1].roughness = 0.4F;
    scene.objects.push_back({"floor", "floor", "plane", "floor", {}, true});
    scene.objects[0].transform.scale = {3, 1, 3};
    scene.objects.push_back({"metal", "metal", "cube", "metal", {}, true});
    scene.objects[1].transform.translation.y = 0.5F;
    for (int kind = 0; kind < 3; ++kind) {
        minirender::Image image(128, 64);
        for (std::size_t y = 0; y < 64; ++y) {
            for (std::size_t x = 0; x < 128; ++x) {
                const double phi = 2 * std::numbers::pi * (static_cast<double>(x) + 0.5) / 128;
                const double theta = std::numbers::pi * (static_cast<double>(y) + 0.5) / 64;
                const glm::dvec3 d{std::sin(theta) * std::cos(phi), std::cos(theta),
                                   std::sin(theta) * std::sin(phi)};
                image.at(x, y) = kind == 0 ? glm::vec3(2, 0.5F, 0.25F)
                                           : glm::vec3(1.0 + (kind == 1 ? 0.5 : 0.3) * d);
            }
        }
        const auto file = path(directory + QString("/env%1.exr").arg(kind));
        image.write_exr(file);
        if (kind) {
            scene.textures.push_back(
                {"env" + std::to_string(kind), file, minirender::ColorEncoding::linear,
                 std::make_shared<const minirender::Texture>(std::move(image))});
        }
    }
    scene.environment_map = {};
    minirender::save_scene(scene, path(directory + "/scene.json"));
    window.load_scene_file(directory + "/scene.json");
    window.viewport()->set_shadows_enabled(false);
    window.viewport()->set_fxaa_enabled(false);
    auto ao = window.viewport()->ssao_settings();
    ao.enabled = false;
    window.viewport()->set_ssao_settings(ao);
}

// 逐光线求几何交点，复用 CPU 数据查询；不从 shader 输出反推法线、反射或颜色。
void verify(Workbench& window, const QString& directory, const char* label) {
    auto* viewport = window.viewport();
    const auto maps = viewport->ibl_maps();
    const auto lut = viewport->ibl_lut();
    require(maps && lut, "IBL data is not ready");
    const auto hdr = viewport->grab_hdr();
    minirender::display_image(hdr, viewport->exposure())
        .write_png(path(directory + "/" + label + ".png"));
    hdr.write_exr(path(directory + "/" + label + ".exr"));
    const auto snapshot = minirender::make_snapshot(window.document());
    const minirender::SceneBvh bvh(snapshot);
    std::array<int, 3> counts{};
    double maximum = 0;
    for (int gy = 0; gy < 24; ++gy) {
        for (int gx = 0; gx < 32; ++gx) {
            const auto x =
                static_cast<std::size_t>((gx + 0.43) * static_cast<double>(hdr.width()) / 32);
            const auto y =
                static_cast<std::size_t>((gy + 0.37) * static_cast<double>(hdr.height()) / 24);
            const auto ray = viewport->camera().generate_ray(
                {(static_cast<double>(x) + 0.5) / static_cast<double>(hdr.width()),
                 (static_cast<double>(y) + 0.5) / static_cast<double>(hdr.height())},
                static_cast<double>(hdr.width()) / static_cast<double>(hdr.height()));
            const auto hit = bvh.intersect(ray);
            const double strength = snapshot->environment_strength,
                         rotation = snapshot->environment_rotation_degrees;
            glm::dvec3 expected;
            if (!hit) {
                ++counts[0];
                expected = viewport->environment_background()
                               ? glm::dvec3(snapshot->environment) +
                                     strength * minirender::sample_ibl_latlong(
                                                    maps->source->level(0), ray.direction, rotation)
                               : glm::dvec3(0.012F, 0.018F, 0.028F);
            } else {
                if (std::min({hit->barycentric.x, hit->barycentric.y, hit->barycentric.z}) < 0.01) {
                    continue;
                }
                const auto& material = snapshot->instances[hit->instance_index].material;
                auto n = glm::normalize(glm::dvec3(hit->shading_normal));
                const auto v =
                    glm::normalize(glm::dvec3(viewport->camera().position) - hit->position);
                if (glm::dot(n, v) < 0) {
                    n = -n;
                }
                if (material.kind == minirender::MaterialKind::diffuse) {
                    ++counts[1];
                    expected = glm::dvec3(material.base_color) *
                               (glm::dvec3(snapshot->environment) +
                                strength * minirender::sample_ibl_diffuse(*maps, n, rotation));
                } else {
                    ++counts[2];
                    const auto ab = minirender::sample_ibl_brdf(*lut, std::max(0.0, glm::dot(n, v)),
                                                                material.roughness);
                    expected =
                        (glm::dvec3(snapshot->environment) +
                         strength * minirender::sample_ibl_specular(*maps, glm::reflect(-v, n),
                                                                    material.roughness, rotation)) *
                        (glm::dvec3(material.base_color) * ab.x + ab.y);
                }
                expected += glm::dvec3(material.emission);
            }
            const double error = glm::length(glm::dvec3(hdr.at(x, y)) - expected);
            maximum = std::max(maximum, error);
            if (error > 0.001 + 0.001 * glm::length(expected)) {
                qInfo() << "IBL bad probe" << label << x << y << "expected" << expected.x
                        << expected.y << expected.z << "actual" << hdr.at(x, y).x << hdr.at(x, y).y
                        << hdr.at(x, y).z;
                throw std::runtime_error("GPU IBL differs from CPU data sampling");
            }
        }
    }
    qInfo() << "IBL GPU/CPU" << label << "background/diffuse/metal" << counts[0] << counts[1]
            << counts[2] << "max error" << maximum;
    require(counts[0] > 10 && counts[1] > 100 && counts[2] > 10,
            "IBL scene did not cover all shading paths");
}
void verify_ao(Workbench& window, const QString& directory) {
    auto* viewport = window.viewport();
    const auto original = viewport->ssao_settings();
    const auto view_mode = viewport->view_mode();
    const auto baseline = viewport->grab_hdr();
    auto settings = original;
    settings.enabled = true;
    settings.filtered = true;
    settings.radius = 0.9F;
    settings.samples = 64;
    viewport->set_ssao_settings(settings);
    const auto masked = viewport->grab_hdr();
    viewport->set_view_mode(5);
    const auto mask = viewport->grab();
    require(baseline.width() == masked.width() && baseline.height() == masked.height(),
            "IBL AO comparison dimensions changed");
    const auto snapshot = minirender::make_snapshot(window.document());
    const minirender::SceneBvh bvh(snapshot);
    double unchanged = 0, composition = 0;
    int darkened = 0;
    for (int gy = 0; gy < 24; ++gy) {
        for (int gx = 0; gx < 32; ++gx) {
            const auto x =
                static_cast<std::size_t>((gx + 0.43) * static_cast<double>(baseline.width()) / 32);
            const auto y =
                static_cast<std::size_t>((gy + 0.37) * static_cast<double>(baseline.height()) / 24);
            const auto ray = viewport->camera().generate_ray(
                {(static_cast<double>(x) + 0.5) / static_cast<double>(baseline.width()),
                 (static_cast<double>(y) + 0.5) / static_cast<double>(baseline.height())},
                static_cast<double>(baseline.width()) / static_cast<double>(baseline.height()));
            const auto hit = bvh.intersect(ray);
            const bool diffuse = hit && snapshot->instances[hit->instance_index].material.kind ==
                                            minirender::MaterialKind::diffuse;
            const auto a = glm::dvec3(baseline.at(x, y)), b = glm::dvec3(masked.at(x, y));
            if (diffuse) {
                const double factor =
                    mask.pixelColor(static_cast<int>(x), static_cast<int>(y)).redF();
                composition = std::max(composition, glm::length(b - a * factor));
                darkened += glm::length(a - b) > 1e-4;
            } else {
                unchanged = std::max(unchanged, glm::length(a - b));
            }
        }
    }
    qInfo() << "IBL AO unchanged specular/background error" << unchanged << "diffuse mask error"
            << composition << "darkened probes" << darkened;
    require(unchanged < 1e-6 && composition < 0.004 && darkened > 3,
            "SSAO did not isolate IBL diffuse lighting");
    masked.write_exr(path(directory + "/ibl-ao.exr"));
    minirender::display_image(masked).write_png(path(directory + "/ibl-ao.png"));
    viewport->set_ssao_settings(original);
    viewport->set_view_mode(view_mode);
}

void verify_panel(Workbench& window) {
    auto* area =
        qobject_cast<QScrollArea*>(window.findChild<QTabWidget*>("inspector_tabs")->widget(5));
    require(area && area->widget()->width() <= area->viewport()->width(),
            "Environment panel requires hidden horizontal scrolling");
    auto* panel = area->widget();
    auto* form = qobject_cast<QFormLayout*>(panel->layout());
    require(form != nullptr, "Environment form layout is missing");
    std::vector<QWidget*> fields;
    for (int i = 0; i < form->count(); ++i) {
        auto* field = form->itemAt(i)->widget();
        if (!field || !field->isVisibleTo(panel)) {
            continue;
        }
        const auto rect = field->geometry();
        require(rect.left() >= 0 && rect.right() < panel->width(),
                "Environment field exceeds panel bounds");
        for (auto* other : fields) {
            const auto overlap = rect.intersected(other->geometry());
            require(overlap.width() <= 1 || overlap.height() <= 1, "Environment fields overlap");
        }
        fields.push_back(field);
    }
    require(fields.size() > 8, "Environment controls are missing");
    auto* bottom = *std::max_element(fields.begin(), fields.end(), [](auto* a, auto* b) {
        return a->geometry().bottom() < b->geometry().bottom();
    });
    area->ensureWidgetVisible(bottom);
    require(
        area->viewport()->rect().contains(bottom->mapTo(area->viewport(), bottom->rect().center())),
        "Environment bottom controls are inaccessible");
}

struct State {
    int step = 0, ticks = 0, hold = 0;
    int maximum_ticks = 1500;
    std::string source_a;
    std::uint64_t environment_jobs = 0, hits = 0, gpu_updates = 0;
    SceneUploadStatistics uploads;
    qint64 last = 0, max_gap = 0;
    bool monitor = false;
    qint64 cancel_started = 0;
    QSize extent;
    int stable = 0;
    QElapsedTimer clock;
};
} // namespace

void start_ibl_smoke(Workbench& window, const QString& directory) {
    require(QDir().mkpath(directory), "Cannot create IBL output directory");
    auto state = std::make_shared<State>();
    bool valid_budget = false;
    const int configured = qEnvironmentVariableIntValue("MINIRENDER_IBL_TEST_TICKS", &valid_budget);
    if (valid_budget && configured > 0 && configured <= 12000) {
        state->maximum_ticks = configured;
    }
    state->clock.start();
    auto* heartbeat = new QTimer(&window);
    heartbeat->setInterval(10);
    QObject::connect(heartbeat, &QTimer::timeout, &window, [state] {
        const auto now = state->clock.elapsed();
        if (state->last && now - state->last > 150) {
            qInfo() << "IBL heartbeat long gap" << now - state->last << "step" << state->step
                    << "monitor" << state->monitor;
        }
        if (state->last && state->monitor) {
            state->max_gap = std::max(state->max_gap, now - state->last);
        }
        state->last = now;
    });
    heartbeat->start();
    auto* timer = new QTimer(&window);
    timer->setInterval(100);
    QObject::connect(
        timer, &QTimer::timeout, &window, [state, timer, heartbeat, &window, directory] {
            try {
                auto* viewport = window.viewport();
                auto* controller = window.ibl_controller();
                require(++state->ticks < state->maximum_ticks && viewport->failure().isEmpty(),
                        "IBL test timed out or GPU failed");
                if (viewport->rendered_frames() < 5 || !viewport->isExposed()) {
                    window.raise();
                    return;
                }
                const auto status = controller->status();
                require(status.state != IblTaskState::failed, "IBL worker failed");
                auto* texture = window.findChild<QComboBox*>("environment_texture");
                const auto select = [&](const std::string& id) {
                    texture->setCurrentIndex(texture->findData(QString::fromStdString(id)));
                };
                const auto verify_frame = [&](const char* label) {
                    verify(window, directory, label);
                    // 显式截图/编码的等待不计入普通交互的 heartbeat 间隔。
                    state->last = state->clock.elapsed();
                };
                switch (state->step) {
                case 0:
                    prepare(window, directory);
                    window.findChild<QTabWidget*>("inspector_tabs")->setCurrentIndex(5);
                    state->last = state->clock.elapsed();
                    state->monitor = true;
                    ++state->step;
                    break;
                case 1:
                    if (status.state != IblTaskState::ready) {
                        return;
                    }
                    window.import_environment_file(directory + "/env0.exr");
                    ++state->step;
                    break;
                case 2:
                    if (window.asset_loading() || status.state != IblTaskState::ready ||
                        !viewport->ibl_maps()) {
                        return;
                    }
                    state->source_a = window.document().environment_map.texture_id;
                    require(!state->source_a.empty(), "Environment import did not select source");
                    for (const auto& material : window.document().materials) {
                        require(material.base_color_texture.empty(),
                                "Environment import changed object albedo");
                    }
                    verify_frame("constant");
                    state->environment_jobs = status.environment_started;
                    state->gpu_updates = viewport->ibl_texture_updates();
                    state->uploads = viewport->upload_statistics();
                    window.findChild<QDoubleSpinBox*>("environment_strength")->setValue(0.7);
                    window.select_object("metal");
                    window.findChild<QDoubleSpinBox*>("roughness")->setValue(1);
                    ++state->step;
                    break;
                case 3:
                    verify_frame("rough");
                    require(viewport->upload_statistics().mesh_uploads ==
                                    state->uploads.mesh_uploads &&
                                viewport->upload_statistics().descriptor_rebuilds ==
                                    state->uploads.descriptor_rebuilds,
                            "IBL parameter edit rebuilt scene geometry or material descriptors");
                    require(status.environment_started == state->environment_jobs &&
                                viewport->ibl_texture_updates() == state->gpu_updates,
                            "Parameter edit rebaked or reuploaded IBL");
                    select("env1");
                    ++state->step;
                    break;
                case 4: {
                    auto* cancel = window.findChild<QPushButton*>("environment_cancel");
                    if (status.state != IblTaskState::preparing || !cancel->isEnabled() ||
                        status.progress.stage != minirender::IblStage::specular) {
                        return;
                    }
                    cancel->click();
                    state->cancel_started = state->clock.elapsed();
                    ++state->step;
                    break;
                }
                case 5:
                    require(state->clock.elapsed() - state->cancel_started < 2000,
                            "IBL cancellation exceeded two seconds");
                    if (status.state != IblTaskState::cancelled) {
                        return;
                    }
                    if (!state->hold++) {
                        state->environment_jobs = status.environment_started;
                    }
                    require(status.environment_started == state->environment_jobs &&
                                !viewport->ibl_maps(),
                            "Cancelled IBL restarted automatically or retained old environment");
                    if (state->hold < 3) {
                        return;
                    }
                    if (!window.findChild<QPushButton*>("environment_retry")->isEnabled()) {
                        return;
                    }
                    window.findChild<QPushButton*>("environment_retry")->click();
                    ++state->step;
                    break;
                case 6:
                    if (status.state != IblTaskState::ready || !viewport->ibl_maps()) {
                        return;
                    }
                    window.findChild<QDoubleSpinBox*>("roughness")->setValue(0.37);
                    window.findChild<QDoubleSpinBox*>("environment_rotation")->setValue(90);
                    ++state->step;
                    break;
                case 7:
                    verify_frame("rotated");
                    state->environment_jobs = status.environment_started;
                    state->gpu_updates = viewport->ibl_texture_updates();
                    window.findChild<QAction*>("undo")->trigger();
                    ++state->step;
                    break;
                case 8:
                    require(window.document().environment_map.rotation_degrees == 0,
                            "Environment undo failed");
                    verify_frame("undo");
                    window.findChild<QAction*>("redo")->trigger();
                    ++state->step;
                    break;
                case 9:
                    require(window.document().environment_map.rotation_degrees == 90,
                            "Environment redo failed");
                    verify_frame("redo");
                    require(status.environment_started == state->environment_jobs &&
                                viewport->ibl_texture_updates() == state->gpu_updates,
                            "Rotation rebuilt IBL data");
                    state->hits = status.cache_hits;
                    select(state->source_a);
                    ++state->step;
                    break;
                case 10:
                    if (status.state != IblTaskState::ready || !viewport->ibl_maps()) {
                        return;
                    }
                    require(status.cache_hits > state->hits &&
                                status.environment_started == state->environment_jobs,
                            "Previously baked source missed cache");
                    verify_frame("cached");
                    select("env2");
                    ++state->step;
                    break;
                case 11:
                    if (status.state != IblTaskState::preparing ||
                        status.environment_started == state->environment_jobs) {
                        return;
                    }
                    select("env1");
                    ++state->step;
                    break;
                case 12:
                    if (status.state != IblTaskState::ready || !viewport->ibl_maps()) {
                        return;
                    }
                    verify_frame("latest");
                    window.save_scene_file(directory + "/saved.json");
                    window.load_scene_file(directory + "/saved.json");
                    ++state->step;
                    break;
                case 13:
                    if (status.state != IblTaskState::ready || !viewport->ibl_maps()) {
                        return;
                    }
                    require(window.document().environment_map.texture_id == "env1" &&
                                window.document().environment_map.rotation_degrees == 90,
                            "Environment persistence failed");
                    verify_frame("reopened");
                    state->environment_jobs = status.environment_started;
                    state->gpu_updates = viewport->ibl_texture_updates();
                    state->extent = viewport->swapChainImageSize();
                    viewport->set_camera(
                        minirender::rotate_camera(viewport->camera(), 0.25F, -0.05F, true));
                    window.resize(1000, 600);
                    ++state->step;
                    break;
                case 14: {
                    const auto current = viewport->swapChainImageSize();
                    const QSize wanted(
                        static_cast<int>(viewport->width() * viewport->devicePixelRatio()),
                        static_cast<int>(viewport->height() * viewport->devicePixelRatio()));
                    if (current == state->extent || current != wanted) {
                        state->stable = 0;
                        return;
                    }
                    if (++state->stable < 2) {
                        return;
                    }
                    verify_frame("camera-resized");
                    verify_panel(window);
                    require(status.environment_started == state->environment_jobs &&
                                viewport->ibl_texture_updates() == state->gpu_updates,
                            "Camera or resize rebuilt IBL textures");
                    window.findChild<QCheckBox*>("environment_background")->setChecked(false);
                    ++state->step;
                    break;
                }
                case 15:
                    verify_frame("background-off");
                    window.findChild<QCheckBox*>("environment_background")->setChecked(true);
                    verify_ao(window, directory);
                    state->last = state->clock.elapsed();
                    select("env2");
                    ++state->step;
                    break;
                case 16:
                    if (status.state != IblTaskState::preparing ||
                        status.environment_started == state->environment_jobs) {
                        return;
                    }
                    require(state->max_gap < 500, "IBL preprocessing blocked the GUI event loop");
                    timer->stop();
                    heartbeat->stop();
                    qInfo() << "IBL smoke PASS: CPU/GPU diffuse/specular/background, roughness, "
                               "strength, rotation, import role, cancel/retry, cache, newest "
                               "source, undo/redo, persistence, camera/resize and active-task "
                               "shutdown; max heartbeat gap"
                            << state->max_gap;
                    QApplication::exit(0);
                    break;
                }
            } catch (const std::exception& error) {
                timer->stop();
                qCritical() << "IBL smoke FAIL:" << error.what();
                QApplication::exit(1);
            }
        });
    timer->start();
}
