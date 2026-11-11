#include <minirender/assets/scene_io.hpp>
#include <minirender/render/sppm.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

using namespace minirender;

// 固定模板应产生真实焦散；镜面版本还必须包含相机侧和光源侧的 delta 事件。
TEST_CASE("Packaged SPPM presets preserve caustic and SDS transport", "[sppm][presets]") {
    for (const auto* name : {"sppm-glass", "sppm-mirror"}) {
        INFO(name);
        const auto scene = load_scene(std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes" /
                                      (std::string(name) + ".json"));
        REQUIRE(scene.objects.size() == 8);
        REQUIRE(scene.media.empty());
        REQUIRE(scene.camera.aperture_radius == 0);
        const auto glass = std::find_if(scene.materials.begin(), scene.materials.end(),
                                        [](const auto& m) { return m.id == "mat_glass_ball"; });
        REQUIRE(glass != scene.materials.end());
        REQUIRE(glass->kind == MaterialKind::dielectric);
        IntegratorSettings settings;
        settings.kind = IntegratorKind::sppm;
        settings.maximum_depth = 8;
        settings.sppm.photons_per_iteration = 2000;
        settings.sppm.initial_radius = .12;
        SppmIntegrator renderer(make_snapshot(scene), {24, 24, 16, 101}, settings);
        for (int i = 0; i < 16; ++i) {
            renderer.advance();
        }
        REQUIRE(renderer.statistics().emitted_photons == 32000);
        REQUIRE(renderer.statistics().specular_light_contributions > 0);
        if (std::string(name) == "sppm-mirror") {
            REQUIRE(renderer.statistics().sds_contributions > 0);
        }
        const auto image = renderer.image();
        double energy = 0;
        for (std::size_t y = 0; y < image.height(); ++y) {
            for (std::size_t x = 0; x < image.width(); ++x) {
                const auto value = image.at(x, y);
                REQUIRE(std::isfinite(value.x));
                REQUIRE(std::isfinite(value.y));
                REQUIRE(std::isfinite(value.z));
                energy += glm::length(value);
            }
        }
        REQUIRE(energy > 1);
    }
}
