// 使用离地高度不同的遮挡物，测量接触半影和多个方向光的独立贡献。
#include "shadow_smoke.hpp"
#include "workbench.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QDoubleSpinBox>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// 沿挡板侧边在接收平面上取样，用无阴影 HDR 归一化。统计 15% 到 85% 的过渡宽度（米）。
float penumbra_width(const minirender::Image& shaded, const minirender::Image& open,
                     const minirender::Camera& camera, const minirender::SceneObject& plate) {
    constexpr int   samples = 250;
    constexpr float span    = 0.70F;
    const float     aspect  = static_cast<float>(open.width()) / static_cast<float>(open.height());
    const auto      matrix  = camera.projection_matrix(aspect) * camera.view_matrix();
    const auto      center  = plate.transform.translation;
    const float     edge    = center.x + plate.transform.scale.x * 0.5F;
    std::array<float, samples> profile{};
    for (int i = 0; i < samples; ++i) {
        const float x    = edge + span * ((static_cast<float>(i) + 0.5F) / samples - 0.5F);
        const auto  clip = matrix * glm::vec4(x, 0, center.z + center.y, 1);
        const auto  uv   = glm::vec2(clip) / clip.w * 0.5F + 0.5F;
        require(uv.x > 0 && uv.x < 1 && uv.y > 0 && uv.y < 1, "Shadow probe outside viewport");
        const auto px    = static_cast<std::size_t>(uv.x * static_cast<float>(open.width()));
        const auto py    = static_cast<std::size_t>(uv.y * static_cast<float>(open.height()));
        const auto value = shaded.at(px, py).g / open.at(px, py).g;
        require(std::isfinite(value), "Non-finite shadow visibility");
        profile[static_cast<std::size_t>(i)] = value;
    }
    const float dark = profile.front();
    require(dark < 0.5F && profile.back() > 0.97F, "Umbra or fully lit ground is missing");
    int transition = 0;
    for (const float value : profile) {
        const float visibility = (value - dark) / (1.0F - dark);
        if (visibility > 0.15F && visibility < 0.85F) {
            ++transition;
        }
    }
    return span * static_cast<float>(transition) / samples;
}

void verify(Workbench& window, const QString& directory) {
    auto* viewport = window.viewport();
    auto* enabled  = window.findChild<QCheckBox*>("shadows_enabled");
    auto* modes    = window.findChild<QComboBox*>("shadow_filter");
    auto* angle    = window.findChild<QDoubleSpinBox*>("shadow_half_angle");
    auto* radius   = window.findChild<QDoubleSpinBox*>("shadow_filter_radius");
    require(enabled && modes && angle && radius, "Missing shadow controls");
    window.findChild<QComboBox*>("view_mode")->setCurrentIndex(0);
    auto       document = window.document();
    const auto plate    = [&](const char* id) {
        return std::find_if(document.objects.begin(), document.objects.end(),
                            [id](const auto& object) { return object.id == id; });
    };
    const auto low  = plate("low_plate");
    const auto high = plate("high_plate");
    require(low != document.objects.end() && high != document.objects.end(),
            "Use scenes/soft-shadows.json for the penumbra measurement");
    window.findChild<QCheckBox*>("fxaa_enabled")->setChecked(false);
    window.findChild<QCheckBox*>("ssao_enabled")->setChecked(false);
    enabled->setChecked(false);
    const auto unshadowed = viewport->grab_hdr();
    enabled->setChecked(true);
    const std::array<const char*, 3> names{"hard", "pcf", "pcss"};
    QImage                           hard;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto mode = static_cast<ShadowFilter>(i);
        modes->setCurrentIndex(static_cast<int>(i));
        const bool adaptive = mode == ShadowFilter::pcss;
        require(angle->isEnabled() == adaptive, "Wrong angular radius control state");
        require(radius->isEnabled() == (!adaptive && mode != ShadowFilter::hard),
                "Wrong fixed radius control state");
        angle->setValue(5.0);
        radius->setValue(8.0);
        const QImage color = viewport->grab();
        require(!color.isNull() && color.save(directory + "/" + names[i] + ".png"),
                "Cannot save shadow image");
        const auto  hdr        = viewport->grab_hdr();
        const float near_width = penumbra_width(hdr, unshadowed, viewport->camera(), *low);
        const float far_width  = penumbra_width(hdr, unshadowed, viewport->camera(), *high);
        qInfo() << names[i] << "penumbra near/far (m)" << near_width << far_width;
        if (mode == ShadowFilter::hard) {
            hard = color;
            require(near_width < 0.02F && far_width < 0.02F, "Hard reference is blurred");
            continue;
        }
        require(color != hard && near_width > 0.003F, "Shadow filter left a hard edge");
        if (adaptive) {
            require(far_width > 1.6F * near_width, "Penumbra does not widen with blocker distance");
            angle->setValue(2.5);
            const auto narrower = viewport->grab_hdr();
            require(penumbra_width(narrower, unshadowed, viewport->camera(), *high) <
                        far_width * 0.85F,
                    "Reducing the angular radius did not narrow the shadow");
            angle->setValue(0);
        } else {
            require(std::abs(far_width - near_width) < 0.035F, "Fixed filter changed with height");
            radius->setValue(4.0);
            const auto narrower = viewport->grab_hdr();
            require(penumbra_width(narrower, unshadowed, viewport->camera(), *high) <
                        far_width * 0.85F,
                    "Reducing the fixed radius did not narrow the shadow");
            radius->setValue(0);
        }
        require(viewport->grab() == hard, "Zero radius differs from hard shadow");
    }

    // 编辑后必须重新生成阴影图；移走低挡板，原阴影中心应恢复直接光。
    angle->setValue(5.0);
    low->visible = false;
    viewport->set_scene(minirender::make_snapshot(document));
    const auto cleared = viewport->grab_hdr();
    const auto center  = low->transform.translation;
    const auto matrix = viewport->camera().projection_matrix(static_cast<float>(cleared.width()) /
                                                             static_cast<float>(cleared.height())) *
                        viewport->camera().view_matrix();
    const auto clip   = matrix * glm::vec4(center.x, 0, center.z + center.y, 1);
    const auto uv     = glm::vec2(clip) / clip.w * 0.5F + 0.5F;
    const auto x      = static_cast<std::size_t>(uv.x * static_cast<float>(cleared.width()));
    const auto y      = static_cast<std::size_t>(uv.y * static_cast<float>(cleared.height()));
    require(cleared.at(x, y).g >= 0.98F * unshadowed.at(x, y).g, "Stale depth shadow after edit");

    // 分别把两盏不同方向的灯放在第零层，再与同时放在第零、第一层的结果对照。
    // 环境项只保留一份，检查不同数组层的深度缓存与光照叠加。
    const auto first     = document.directional_lights.front();
    auto       second    = first;
    second.id            = "second";
    second.direction     = glm::normalize(glm::vec3(-1, -2, -1));
    second.color         = {0.65F, 0.8F, 1.0F};
    second.intensity     = 1.7F;
    const auto lit_frame = [&](std::vector<minirender::DirectionalLight> lights) {
        document.directional_lights = std::move(lights);
        viewport->set_scene(minirender::make_snapshot(document));
        return viewport->grab_hdr();
    };
    for (const auto filter : {ShadowFilter::pcf, ShadowFilter::pcss}) {
        modes->setCurrentIndex(static_cast<int>(filter));
        radius->setValue(8.0);
        const auto ambient = lit_frame({});
        const auto a       = lit_frame({first});
        const auto b       = lit_frame({second});
        const auto both    = lit_frame({first, second});
        float      error   = 0;
        for (std::size_t py = 0; py < both.height(); ++py) {
            for (std::size_t px = 0; px < both.width(); ++px) {
                const auto delta =
                    glm::abs(both.at(px, py) - (a.at(px, py) + b.at(px, py) - ambient.at(px, py)));
                error = std::max(error, std::max({delta.r, delta.g, delta.b}));
            }
        }
        qInfo() << (filter == ShadowFilter::pcf ? "PCF" : "PCSS") << "multi-light HDR error"
                << error;
        require(error < 0.00005F, "Shadow depth layers do not match isolated lights");
    }
    // 同一帧左侧 PCSS、右侧硬阴影。重新选择过滤器只能使右侧缓存过期。
    window.findChild<QComboBox*>("compare_right")->setCurrentIndex(1);
    auto* reference = window.findChild<QComboBox*>("reference_shadow");
    reference->setCurrentIndex(1);
    const auto adaptive = viewport->grab_comparison_side(0);
    require(adaptive != viewport->grab_comparison_side(1), "A/B shadow filter override missing");
    const auto draws = viewport->comparison_draws();
    reference->setCurrentIndex(3);
    require(adaptive == viewport->grab_comparison_side(1) &&
                viewport->comparison_draws() == draws + 1,
            "A/B shadow cache invalidation incorrect");
    window.findChild<QComboBox*>("compare_right")->setCurrentIndex(0);
    for (int mode = 8; mode <= 11; ++mode) {
        window.findChild<QComboBox*>("view_mode")->setCurrentIndex(mode);
        const auto debug = viewport->grab_hdr();
        float      min = 1.F, max = 0.F;
        for (std::size_t row = 0; row < debug.height(); row += 3) {
            for (std::size_t column = 0; column < debug.width(); column += 3) {
                const auto v = debug.at(column,row).r;
                require(std::isfinite(v) && v >= 0 && v <= 1, "Invalid shadow diagnostic value");
                min = std::min(min, v);
                max = std::max(max, v);
            }
        }
        require(max - min > 0.01F, "Empty shadow diagnostic");
    }
}

} // namespace

void start_shadow_smoke(Workbench& window, const QString& directory) {
    require(QDir().mkpath(directory), "Cannot create soft shadow output directory");
    auto* timer = new QTimer(&window);
    QObject::connect(timer, &QTimer::timeout, &window, [timer, &window, directory] {
        if (!window.viewport()->isValid() || window.viewport()->rendered_frames() < 2) {
            return;
        }
        timer->stop();
        try {
            verify(window, directory);
            qInfo() << "Shadow smoke PASS";
            QApplication::exit(0);
        } catch (const std::exception& error) {
            qCritical() << "Shadow smoke FAIL:" << error.what();
            QApplication::exit(1);
        }
    });
    timer->start(100);
}
