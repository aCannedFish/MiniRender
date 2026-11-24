// GPU 的矩形发光面与独立 CPU 球面积分逐点比较，同时检查灯具的对象事务。
#include "rectangle_smoke.hpp"
#include "workbench_internal.hpp"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QTimer>
#include <minirender/assets/scene_io.hpp>
#include <minirender/render/bvh.hpp>
#include <minirender/render/path_tracer.hpp>
#include <minirender/render/rectangle_light.hpp>

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

void compare(VulkanViewport& viewport) {
    const auto scene = viewport.scene();
    require(scene && !scene->rectangles.empty(), "No editable rectangular light found");
    const auto           hdr = viewport.grab_hdr();
    const auto&          cam = viewport.camera();
    minirender::SceneBvh bvh(scene);
    double               maximum = 0;
    std::size_t          tested  = 0;
    for (std::size_t y = 4; y < hdr.height(); y += 11) {
        for (std::size_t x = 4; x < hdr.width(); x += 11) {
            const auto ray = cam.generate_ray(
                {(double(x) + .5) / double(hdr.width()), (double(y) + .5) / double(hdr.height())},
                double(hdr.width()) / double(hdr.height()));
            auto hit = bvh.intersect(ray);
            if (!hit ||
                std::min({hit->barycentric.x, hit->barycentric.y, hit->barycentric.z}) < .05) {
                continue;
            }
            const auto& m = scene->instances[hit->instance_index].material;
            if (glm::length(m.emission) > 0) {
                continue;
            }
            auto n = glm::dvec3(hit->shading_normal);
            if (glm::dot(n, -ray.direction) < 0) {
                n = -n;
            }
            glm::dvec3 expected(0);
            for (const auto& light : scene->rectangles) {
                auto contribution = minirender::evaluate_rectangle(
                    light, hit->position, n, -ray.direction, m.base_color, m.roughness,
                    m.kind == minirender::MaterialKind::conductor);
                expected += contribution.diffuse + contribution.specular;
            }
            if (glm::length(expected) < .03) {
                continue;
            }
            maximum = std::max(maximum, glm::length(expected - glm::dvec3(hdr.at(x, y))) /
                                            std::max(1., glm::length(expected)));
            ++tested;
        }
    }
    qInfo() << "Rectangle reference pixels" << tested << "max normalized RGB error" << maximum;
    require(tested > 100 && maximum < .015, "LTC GPU illumination differs from CPU");
}

// 用 CPU 射线判断九个灯面样本是否可见，检查阴影覆盖与部分遮挡的过渡区域。
void verify_visibility(Workbench& window) {
    auto&       viewport = *window.viewport();
    const auto  scene    = viewport.scene();
    const auto& light    = scene->rectangles.front();
    viewport.set_area_shadows(false);
    const auto clear = viewport.grab_hdr();
    viewport.set_area_shadows(true);
    const auto shadow = viewport.grab_hdr();
    const auto draws  = viewport.area_shadow_updates();
    viewport.grab_hdr();
    require(draws == viewport.area_shadow_updates(), "Static light regenerated depth maps");
    minirender::SceneBvh bvh(scene);
    std::size_t          total = 0, partial = 0, dark = 0;
    double               error = 0;
    for (std::size_t y = 3; y < clear.height(); y += 5) {
        for (std::size_t x = 3; x < clear.width(); x += 5) {
            const auto ray =
                viewport.camera().generate_ray({(double(x) + .5) / double(clear.width()),
                                                (double(y) + .5) / double(clear.height())},
                                               double(clear.width()) / double(clear.height()));
            const auto hit = bvh.intersect(ray);
            if (!hit || scene->instances[hit->instance_index].object_id != "floor" ||
                clear.at(x, y).r < .01F) {
                continue;
            }
            double     weights = 0, visible = 0;
            const auto u = glm::dvec3(light.corners[1] - light.corners[0]),
                       v = glm::dvec3(light.corners[3] - light.corners[0]);
            const auto n = glm::normalize(glm::cross(u, v));
            for (int j = 0; j < 3; ++j) {
                for (int i = 0; i < 3; ++i) {
                    auto source =
                        glm::dvec3(light.corners[0]) + u * ((i + .5) / 3) + v * ((j + .5) / 3);
                    auto   delta    = source - hit->position;
                    double distance = glm::length(delta);
                    auto   dir      = delta / distance;
                    double w        = std::max(0., glm::dot(glm::dvec3(hit->shading_normal), dir)) *
                                      std::abs(glm::dot(n, dir)) / (distance * distance);
                    weights += w;
                    const auto blocker = bvh.intersect(minirender::spawn_ray(*hit, dir));
                    if (!blocker || blocker->distance >= distance - .001) {
                        visible += w;
                    }
                }
            }
            if (weights <= 0) {
                continue;
            }
            const auto expected = visible / weights;
            const auto actual   = shadow.at(x, y).r / clear.at(x, y).r;
            if (actual < .05F) {
                ++dark;
            }
            if (actual > .15F && actual < .85F) {
                ++partial;
            }
            error += std::abs(expected - actual);
            ++total;
        }
    }
    qInfo() << "Area shadow floor/umbra/penumbra" << total << dark << partial
            << "mean visibility error" << error / std::max(1., double(total));
    require(total > 100 && partial > 10 && dark > 5 && error / double(total) < .065,
            "Rectangle shadow differs from CPU visibility");
    const QPointF pos(viewport.width() * .1, viewport.height() * .15);
    QMouseEvent   press(QEvent::MouseButtonPress, pos, pos, pos, Qt::RightButton, Qt::RightButton,
                        Qt::NoModifier);
    QApplication::sendEvent(&viewport, &press);
    viewport.grab_hdr();
    require(viewport.area_shadow_grid() == 2, "Interactive shadow sample count incorrect");
    QMouseEvent release(QEvent::MouseButtonRelease, pos, pos, pos, Qt::RightButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(&viewport, &release);
    viewport.grab_hdr();
    require(viewport.area_shadow_grid() == 3, "Full shadow quality did not return");
    const auto stable = viewport.area_shadow_updates();
    window.select_object("short_box");
    auto* position = window.findChild<QDoubleSpinBox*>("translation.x");
    position->setValue(position->value() + .25);
    const auto moved = viewport.grab_hdr();
    require(viewport.area_shadow_updates() == stable + 1,
            "Moving an occluder left stale depth maps");
    double changed = 0;
    for (std::size_t y = 0; y < moved.height(); y += 5) {
        for (std::size_t x = 0; x < moved.width(); x += 5) {
            changed += glm::length(moved.at(x, y) - shadow.at(x, y));
        }
    }
    require(changed > 1, "Moving occluder did not change surface illumination");
    window.findChild<QAction*>("undo")->trigger();
    viewport.set_area_shadows(false);
}

} // namespace

void start_rectangle_smoke(Workbench& window, const QString& directory) {
    auto* timer = new QTimer(&window);
    QObject::connect(timer, &QTimer::timeout, &window, [&window, timer, directory] {
        if (!window.viewport()->isValid() || window.viewport()->rendered_frames() < 3) {
            return;
        }
        timer->stop();
        try {
            window.findChild<QComboBox*>("view_mode")->setCurrentIndex(0);
            window.findChild<QCheckBox*>("ssao_enabled")->setChecked(false);
            window.viewport()->set_area_shadows(false);
            compare(*window.viewport());
            verify_visibility(window);
            window.select_object("short_box");
            window.findChild<QComboBox*>("material_kind")
                ->setCurrentIndex(static_cast<int>(minirender::MaterialKind::conductor));
            window.findChild<QDoubleSpinBox*>("roughness")->setValue(.6);
            compare(*window.viewport());
            window.select_object("lamp");
            auto* x = window.findChild<QDoubleSpinBox*>("translation.x");
            x->setValue(x->value() + .25);
            compare(*window.viewport());
            const auto count = window.document().objects.size();
            window.findChild<QPushButton*>("add_rectangle")->click();
            require(window.document().objects.size() == count + 1 &&
                        window.viewport()->scene()->rectangles.size() == 2,
                    "Creating rectangular light failed");
            const auto file = directory + "/rectangles.json";
            window.save_scene_file(file);
            window.load_scene_file(file);
            require(window.viewport()->scene()->rectangles.size() == 2,
                    "Rectangle not preserved by scene IO");
            compare(*window.viewport());
            window.viewport()->grab().save(directory + "/rectangles.png");
            qInfo() << "Rectangle smoke PASS";
            QApplication::exit(0);
        } catch (const std::exception& error) {
            qCritical() << "Rectangle smoke FAIL:" << error.what();
            QApplication::exit(1);
        }
    });
    timer->start(80);
}
