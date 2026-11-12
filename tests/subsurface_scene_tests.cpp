#include <minirender/assets/scene_io.hpp>
#include <minirender/render/path_tracer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

using namespace minirender;

// 静态模板的单位、光学参数和真实路径记录独立于 Qt 菜单元数据检查。
TEST_CASE("Subsurface presets include millimeter optics and actual surface transport",
          "[sss][presets]") {
    for (const auto* name : {"sss-marble", "sss-wax", "sss-marble-edge", "sss-wax-edge"}) {
        INFO(name);
        const auto scene = load_scene(std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes" /
                                      (std::string(name) + ".json"));
        const bool edge = std::string(name).ends_with("edge");
        const bool marble = std::string(name).find("marble") != std::string::npos;
        REQUIRE(scene.objects.size() == (edge ? 2 : 8));
        REQUIRE(scene.camera.near_plane < .002F);
        const auto material =
            std::find_if(scene.materials.begin(), scene.materials.end(),
                         [](const auto& m) { return m.kind == MaterialKind::subsurface; });
        REQUIRE(material != scene.materials.end());
        REQUIRE(material->subsurface);
        REQUIRE(material->subsurface->density == (marble ? 1 : .5));
        REQUIRE(material->subsurface->reduced_scattering.x == (marble ? 2190 : 450));
        PathStatistics statistics;
        const auto image = PathTracer(make_snapshot(scene), {.maximum_depth = 8})
                               .render({16, 16, 16, 101}, &statistics);
        REQUIRE(statistics.subsurface_events > 0);
        REQUIRE(statistics.subsurface_distance > 0);
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
