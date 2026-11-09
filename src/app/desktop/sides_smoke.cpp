#include "sides_smoke.hpp"
#include "workbench.hpp"

#include <minirender/assets/scene_io.hpp>
#include <minirender/render/path_tracer.hpp>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QTimer>

#include <array>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// 所有场景保留接触面的 y=0。发光颜色用于独立核对实际可见物体，隔离光照误差。
struct Scenario {
    const char* name;
    glm::vec3 position;
    bool plane_only = false;
    bool double_sided = false;
    bool mirror_x = false;
    bool mirror_y = false;
    bool expect_both = true;
};
const std::array scenarios{Scenario{"above", {3, 2.2F, 4.5F}},
                           Scenario{"below", {3, -2, 4.5F}},
                           Scenario{"below-moved", {-3, -1, 3}},
                           Scenario{"below-near", {0, -0.02F, 3}},
                           Scenario{"mirrored", {3, -2, 4.5F}, false, false, true},
                           Scenario{"plane-back", {3, -2, 4.5F}, true},
                           Scenario{"plane-front", {3, 2, 4.5F}, true},
                           Scenario{"plane-double", {3, -2, 4.5F}, true, true},
                           Scenario{"plane-flipped-front", {3, -2, 4.5F}, true, false, false, true},
                           Scenario{"plane-flipped-back", {3, 2, 4.5F}, true, false, false, true}};

void install(Workbench& window, const minirender::SceneDocument& source, const QString& directory,
             const Scenario& scenario) {
    auto scene = source;
    scene.environment = glm::vec3(0);
    scene.environment_map = {};
    scene.directional_lights.clear();
    scene.camera.position = scenario.position;
    scene.camera.target = {0, 0.3F, 0};
    for (auto& material : scene.materials) {
        material.base_color = glm::vec3(0);
        material.base_color_texture.clear();
        material.double_sided = false;
        material.emission = material.id == "floor"
                                ? glm::vec3(0.4F)
                                : (material.id == "blue" ? glm::vec3(0.02F, 0.25F, 0.8F)
                                                         : glm::vec3(0.8F, 0.4F, 0.02F));
    }
    for (auto& object : scene.objects) {
        object.visible = !scenario.plane_only || object.id == "ground";
        if (scenario.mirror_x) {
            object.transform.scale.x *= -1;
        }
        if (scenario.mirror_y) {
            object.transform.scale.y *= -1;
        }
    }
    const auto path = directory + "/fixture.json";
    minirender::save_scene(scene, std::filesystem::path(path.toStdU16String()));
    window.load_scene_file(path);
    window.select_object("ground");
    auto* sided = window.findChild<QCheckBox*>("material_double_sided");
    require(sided && !sided->isChecked(), "Single sided material did not reach the inspector");
    if (scenario.double_sided) {
        sided->setChecked(true);
        require(window.document().materials.front().double_sided, "Sidedness UI edit failed");
        window.findChild<QAction*>("undo")->trigger();
        require(!sided->isChecked(), "Sidedness undo failed");
        window.findChild<QAction*>("redo")->trigger();
        require(sided->isChecked(), "Sidedness redo failed");
        window.save_scene_file(directory + "/double-saved.json");
        window.load_scene_file(directory + "/double-saved.json");
        require(window.document().materials.front().double_sided, "Sidedness save/reopen failed");
    }
    window.findChild<QCheckBox*>("fxaa_enabled")->setChecked(false);
}

// 使用 BVH 查询检查像素中央的可见物体，再核对显示编码。跳过轮廓附近的样本。
// CPU 核心另有解析朝向用例，因此此处可用于验证 Vulkan 的绕序和数据传递。
void verify(Workbench& window, const Scenario& scenario, int mode, const QImage& image) {
    const auto snapshot = window.viewport()->scene();
    const minirender::SceneBvh bvh(snapshot);
    const minirender::PathTracer tracer(snapshot, {.maximum_depth = 0});
    const auto& camera = window.viewport()->camera();
    const double aspect = static_cast<double>(image.width()) / image.height();
    const auto trace = [&](int x, int y) {
        return bvh.intersect(
            camera.generate_ray({(x + 0.5) / image.width(), (y + 0.5) / image.height()}, aspect));
    };
    int matched = 0;
    int checked = 0;
    std::array<int, 3> object_samples{};
    const auto hdr = mode == 0 ? std::optional(window.viewport()->grab_hdr()) : std::nullopt;
    for (int y = 5; y + 5 < image.height(); y += 23) {
        for (int x = 5; x + 5 < image.width(); x += 23) {
            const auto hit = trace(x, y);
            bool stable = true;
            for (const auto offset : {QPoint(-3, 0), QPoint(3, 0), QPoint(0, -3), QPoint(0, 3)}) {
                const auto nearby = trace(x + offset.x(), y + offset.y());
                stable = stable && nearby.has_value() == hit.has_value();
                if (hit && nearby) {
                    stable = stable && nearby->instance_index == hit->instance_index &&
                             nearby->triangle_index == hit->triangle_index;
                }
            }
            if (!stable) {
                continue;
            }
            const auto ray = camera.generate_ray(
                {(x + 0.5) / image.width(), (y + 0.5) / image.height()}, aspect);
            const auto picked = minirender::pick_object(*snapshot, ray);
            require(picked.has_value() == hit.has_value(), "Picking disagrees with CPU visibility");
            glm::vec3 expected(0);
            if (hit) {
                const auto& instance = snapshot->instances[hit->instance_index];
                require(picked == instance.object_id, "Picking selected a hidden surface");
                const std::size_t object = instance.object_id == "ground"
                                               ? 0
                                               : (instance.object_id == "blue_cube" ? 1 : 2);
                ++object_samples[object];
                if (mode == 0) {
                    const auto emission = instance.material.emission;
                    minirender::Sampler sampler(101);
                    require(glm::length(tracer.trace(ray, sampler) - glm::dvec3(emission)) < 1e-6,
                            "CPU path emission disagrees with visible material");
                    require(glm::length(
                                hdr->at(static_cast<std::size_t>(x), static_cast<std::size_t>(y)) -
                                emission) < 2e-5F,
                            "GPU HDR contains a hidden or incorrect material");
                    expected = emission / (glm::vec3(1) + emission);
                } else if (mode == 1) {
                    auto normal = hit->shading_normal;
                    if (glm::dot(glm::dvec3(normal), ray.direction) > 0) {
                        normal = -normal;
                    }
                    expected = normal * 0.5F + 0.5F;
                } else {
                    const auto point = glm::vec4(glm::vec3(hit->position), 1);
                    expected = glm::vec3(1 / (1 - (camera.view_matrix() * point).z));
                }
            } else if (mode == 0) {
                const glm::vec3 background{0.012F, 0.018F, 0.028F};
                expected = background / (glm::vec3(1) + background);
            }
            const auto actual = image.pixelColor(x, y);
            const std::array channels{actual.red(), actual.green(), actual.blue()};
            bool equal = true;
            for (int channel = 0; channel < 3; ++channel) {
                const int encoded =
                    minirender::encode_unorm8(minirender::linear_to_srgb(expected[channel]));
                equal =
                    equal && std::abs(encoded - channels[static_cast<std::size_t>(channel)]) <= 2;
            }
            ++checked;
            matched += equal;
        }
    }
    require(checked > 100 && matched == checked,
            "GPU sidedness image disagrees with CPU reference");
    if (!scenario.plane_only && scenario.position.y < 0) {
        require(object_samples[0] == 0, "Single sided ground remained visible from below");
        if (scenario.expect_both) {
            require(object_samples[1] > 5 && object_samples[2] > 5,
                    "Below-ground fixture did not expose both cube bottoms");
        }
    }
    qInfo() << "Sides" << scenario.name << "mode" << mode << "matched" << matched << "/" << checked
            << "ground/blue/metal" << object_samples;
}

struct State {
    minirender::SceneDocument source;
    std::size_t scenario = 0;
    int mode = 0;
    int ticks = 0;
    int motion = -1;
};

} // namespace

void start_sides_smoke(Workbench& window, const QString& directory) {
    require(QDir().mkpath(directory), "Cannot create sidedness output directory");
    const auto state = std::make_shared<State>();
    state->source = window.document();
    install(window, state->source, directory, scenarios.front());
    auto* timer = new QTimer(&window);
    timer->setInterval(150);
    QObject::connect(timer, &QTimer::timeout, &window, [state, timer, &window, directory] {
        try {
            auto* viewport = window.viewport();
            require(++state->ticks < 400, "Sidedness regression timed out");
            require(viewport->failure().isEmpty(), "Vulkan sidedness renderer failed");
            if (!viewport->isValid() || viewport->rendered_frames() < 5) {
                return;
            }
            if (state->motion >= 0) {
                const Scenario scenario{
                    "motion", viewport->camera().position, false, false, false, false, false};
                const auto image = viewport->grab();
                verify(window, scenario, state->mode, image);
                if (++state->motion == 24) {
                    timer->stop();
                    qInfo() << "Sides smoke PASS: 10 views, 24 camera updates, single/double "
                               "sided, mirrors, wire, CPU/GPU visibility, picking, history and IO";
                    QApplication::exit(0);
                    return;
                }
                auto camera = viewport->camera();
                const float angle = static_cast<float>(state->motion) * 0.27F;
                camera.position = {3.5F * std::sin(angle), -0.5F - 0.4F * std::sin(angle),
                                   3.5F * std::cos(angle)};
                viewport->set_camera(camera);
                state->mode = state->motion % 3;
                window.findChild<QComboBox*>("view_mode")->setCurrentIndex(state->mode);
                return;
            }
            const auto& scenario = scenarios[state->scenario];
            const auto image = viewport->grab();
            require(!image.isNull(), "Sidedness readback is empty");
            const auto name = QString::fromUtf8(scenario.name) + "-" + QString::number(state->mode);
            require(image.save(directory + "/" + name + ".png"), "Cannot save sidedness image");
            if (state->mode < 3) {
                verify(window, scenario, state->mode, image);
            } else if (scenario.plane_only) {
                int lines = 0;
                for (int y = 0; y < image.height(); ++y) {
                    for (int x = 0; x < image.width(); ++x) {
                        lines += image.pixelColor(x, y).red() > 200;
                    }
                }
                const bool visible =
                    scenario.double_sided || ((scenario.position.y > 0) != scenario.mirror_y);
                require(visible ? lines > 10 : lines == 0, "Wireframe retained a hidden back face");
                qInfo() << "Sides wire" << scenario.name << "pixels" << lines;
            }
            ++state->mode;
            if (state->mode == 4) {
                state->mode = 0;
                ++state->scenario;
                if (state->scenario == scenarios.size()) {
                    install(window, state->source, directory, scenarios[1]);
                    state->motion = 0;
                    window.findChild<QComboBox*>("view_mode")->setCurrentIndex(0);
                    return;
                }
                install(window, state->source, directory, scenarios[state->scenario]);
            }
            window.findChild<QComboBox*>("view_mode")->setCurrentIndex(state->mode);
        } catch (const std::exception& error) {
            timer->stop();
            qCritical() << "Sides smoke FAIL:" << error.what();
            QApplication::exit(1);
        }
    });
    timer->start();
}
