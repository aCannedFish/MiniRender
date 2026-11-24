// 核对 G-buffer 基色、屏幕命中位置、分层遍历、一次反弹与直接光合成。
#include "screen_transport_smoke.hpp"
#include "workbench.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QTimer>
#include <minirender/render/bvh.hpp>

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

void verify(Workbench& window, const QString& directory) {
    auto* vp   = window.viewport();
    auto* mode = window.findChild<QComboBox*>("view_mode");
    window.findChild<QCheckBox*>("ssao_enabled")->setChecked(false);
    window.findChild<QCheckBox*>("area_shadows")->setChecked(false);
    auto* hiz    = window.findChild<QCheckBox*>("screen_hiz");
    auto  config = vp->screen_trace();
    config.z     = static_cast<float>(vp->swapChainImageSize().width() +
                                      vp->swapChainImageSize().height() + 64);
    vp->set_screen_trace(config, false);
    mode->setCurrentIndex(0);
    const auto direct = vp->grab_hdr();
    mode->setCurrentIndex(14);
    const auto flat = vp->grab_hdr();
    vp->set_screen_trace(config, true);
    const auto  hierarchy = vp->grab_hdr();
    std::size_t hits = 0, mismatch = 0;
    std::size_t only_flat = 0, only_hiz = 0;
    for (std::size_t y = 0; y < flat.height(); y += 3) {
        for (std::size_t x = 0; x < flat.width(); x += 3) {
            const auto a = flat.at(x, y), b = hierarchy.at(x, y);
            if (a.b > .5F || b.b > .5F) {
                only_flat += a.b > .5F && b.b < .5F;
                only_hiz += b.b > .5F && a.b < .5F;
                ++hits;
                if (a.b != b.b || glm::length(glm::vec2(a) - glm::vec2(b)) > .01F) {
                    ++mismatch;
                }
            }
        }
    }
    qInfo() << "Screen trace hits/disagreement" << hits << mismatch << "only flat/Hi-Z" << only_flat
            << only_hiz;
    require(hits > 100 && mismatch * 100 < hits * 4, "Hi-Z and pixel traversal disagree");
    mode->setCurrentIndex(18);
    const auto accelerated = vp->grab_hdr();
    vp->set_screen_trace(config, false);
    const auto stepped    = vp->grab_hdr();
    double     flat_steps = 0, hiz_steps = 0;
    for (std::size_t y = 0; y < flat.height(); y += 5) {
        for (std::size_t x = 0; x < flat.width(); x += 5) {
            flat_steps += stepped.at(x, y).r;
            hiz_steps += accelerated.at(x, y).r;
        }
    }
    qInfo() << "Hi-Z / pixel traversal steps" << hiz_steps / std::max(.001, flat_steps);
    require(flat_steps > 0 && hiz_steps < flat_steps, "Hi-Z did not reduce traversal steps");
    vp->set_screen_trace(config, true);
    mode->setCurrentIndex(15);
    const auto indirect = vp->grab_hdr();
    mode->setCurrentIndex(16);
    const auto combined = vp->grab_hdr();
    double     peak = 0, error = 0;
    for (std::size_t y = 0; y < combined.height(); y += 5) {
        for (std::size_t x = 0; x < combined.width(); x += 5) {
            const auto add = indirect.at(x, y);
            require(std::isfinite(add.r + add.g + add.b) &&
                        glm::all(glm::greaterThanEqual(add, glm::vec3(0))),
                    "Invalid indirect radiance");
            peak           = std::max(peak, double(glm::length(add)));
            const auto ref = direct.at(x, y) + add;
            error          = std::max(error, double(glm::length(ref - combined.at(x, y))) /
                                                 (1 + glm::length(glm::dvec3(ref))));
        }
    }
    qInfo() << "Indirect peak/composition error" << peak << error;
    require(peak > .02 && error < .005, "Indirect light composition failed");
    vp->grab().save(directory + "/ssgi.png");
    mode->setCurrentIndex(13);
    require(!vp->grab().isNull(), "Mirror probe readback failed");
    mode->setCurrentIndex(17);
    auto                 albedo = vp->grab_hdr();
    auto                 scene  = vp->scene();
    minirender::SceneBvh bvh(scene);
    const auto&          camera  = vp->camera();
    int                  checked = 0;
    for (std::size_t y = 5; y < albedo.height(); y += 19) {
        for (std::size_t x = 5; x < albedo.width(); x += 19) {
            auto ray = camera.generate_ray({(double(x) + .5) / double(albedo.width()),
                                            (double(y) + .5) / double(albedo.height())},
                                           double(albedo.width()) / double(albedo.height()));
            auto hit = bvh.intersect(ray);
            if (!hit ||
                std::min({hit->barycentric.x, hit->barycentric.y, hit->barycentric.z}) < .04) {
                continue;
            }
            const auto& item = scene->instances[hit->instance_index];
            if (item.base_color_texture) {
                continue;
            }
            require(glm::length(albedo.at(x, y) - item.material.base_color) < .003F,
                    "G-buffer albedo is not linear base color");
            ++checked;
        }
    }
    if (checked <= 20) {
        const auto mapped =
            std::find_if(scene->instances.begin(), scene->instances.end(),
                         [](const auto& item) { return bool(item.normal_texture); });
        require(mapped != scene->instances.end(), "Insufficient albedo samples");
        window.select_object(mapped->object_id);
        mode->setCurrentIndex(1);
        const auto active = vp->grab();
        window.findChild<QComboBox*>("normal_texture")->setCurrentIndex(0);
        require(active != vp->grab(), "Normal map has no visible effect");
    }
    hiz->setChecked(true);
}
} // namespace

void start_transport_smoke(Workbench& window, const QString& directory) {
    auto* timer = new QTimer(&window);
    QObject::connect(timer, &QTimer::timeout, &window, [timer, &window, directory] {
        if (!window.viewport()->isValid() || window.viewport()->rendered_frames() < 3) {
            return;
        }
        timer->stop();
        try {
            verify(window, directory);
            qInfo() << "Transport smoke PASS";
            QApplication::exit(0);
        } catch (const std::exception& error) {
            qCritical() << "Transport smoke FAIL:" << error.what();
            QApplication::exit(1);
        }
    });
    timer->start(80);
}
