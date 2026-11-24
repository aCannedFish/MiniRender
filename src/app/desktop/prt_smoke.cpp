// 使用 CPU BVH 的重心坐标重建同一面角传输，逐点核对 Vulkan 的 PRT 着色。
#include "prt_smoke.hpp"
#include "workbench_internal.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <minirender/render/bvh.hpp>

namespace {
void require(bool ok, const char* message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}

void compare(VulkanViewport& viewport) {
    auto scene = viewport.scene();
    auto bake  = viewport.prt();
    require(bake && viewport.view_mode() == VulkanViewport::prt_mode, "PRT bake not displayed");
    auto                 image          = viewport.grab_hdr();
    const auto&          camera         = viewport.camera();
    const auto           expected_light = minirender::scene_sh_light(*scene);
    minirender::SceneBvh bvh(scene);
    double               maximum = 0;
    unsigned             samples = 0;
    for (std::size_t y = 5; y < image.height(); y += 9) {
        for (std::size_t x = 5; x < image.width(); x += 9) {
            const auto ray = camera.generate_ray({(double(x) + .5) / double(image.width()),
                                                  (double(y) + .5) / double(image.height())},
                                                 double(image.width()) / double(image.height()));
            auto       hit = bvh.intersect(ray);
            if (!hit ||
                std::min({hit->barycentric.x, hit->barycentric.y, hit->barycentric.z}) < .02) {
                continue;
            }
            glm::vec3  transfer(0);
            const auto offset = bake->offsets[hit->instance_index] + 3 * hit->triangle_index;
            for (int j = 0; j < 3; ++j) {
                transfer += float(hit->barycentric[j]) *
                            minirender::prt_irradiance(
                                bake->vertices[offset + static_cast<unsigned>(j)], expected_light);
            }
            const auto& m        = scene->instances[hit->instance_index].material;
            auto        expected = transfer * m.base_color + m.emission;
            maximum = std::max(maximum, double(glm::length(expected - image.at(x, y))));
            ++samples;
        }
    }
    require(samples > 50 && maximum < .025, "GPU PRT does not match CPU vertex transfer");
    qInfo() << "PRT pixels/max error" << samples << maximum;
}
} // namespace

void start_prt_smoke(Workbench& window, const QString& directory) {
    auto  stage = std::make_shared<int>(0);
    auto* timer = new QTimer(&window);
    QObject::connect(timer, &QTimer::timeout, &window, [stage, timer, &window, directory] {
        try {
            auto* vp = window.viewport();
            if (!vp->isValid() || vp->rendered_frames() < 3) {
                return;
            }
            auto* start = window.findChild<QPushButton*>("prt_bake");
            auto* mode  = window.findChild<QComboBox*>("prt_method");
            if (!start->isEnabled()) {
                return;
            }
            if (*stage == 0) {
                window.findChild<QSpinBox*>("prt_samples")->setValue(64);
                mode->setCurrentIndex(0);
                start->click();
            } else if (*stage < 3) {
                compare(*vp);
                mode->setCurrentIndex(*stage);
                start->click();
            } else if (*stage == 3) {
                compare(*vp);
                start->click();
            } else {
                require(window.findChild<QLabel*>("prt_status")->text().contains("载入"),
                        "PRT bake cache not reused");
                compare(*vp);
                vp->grab().save(directory + "/prt.png");
                window.select_object(window.document().objects.front().id);
                auto* x = window.findChild<QDoubleSpinBox*>("translation.x");
                x->setValue(x->value() + .15);
                require(!vp->prt(), "Outdated transport remained after geometry edit");
                timer->stop();
                qInfo() << "Prt smoke PASS";
                QApplication::exit(0);
                return;
            }
            ++*stage;
        } catch (const std::exception& error) {
            timer->stop();
            qCritical() << "Prt smoke FAIL:" << error.what();
            QApplication::exit(1);
        }
    });
    timer->start(80);
}
