// GPU 的矩形发光面与独立 CPU 球面积分逐点比较，同时检查灯具的对象事务。
#include "rectangle_smoke.hpp"
#include "workbench_internal.hpp"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QTimer>
#include <minirender/assets/scene_io.hpp>
#include <minirender/render/bvh.hpp>
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
            compare(*window.viewport());
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
