#include <minirender/assets/scene_io.hpp>
#include <minirender/render/path_tracer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace minirender;

// 静态随包模板具有独立的期望结构，防止 GUI 用同一加载器比较时漏掉资产错误。
TEST_CASE("Volume presets include complete geometry media and actual scattering",
          "[volume][presets]") {
    const auto root = std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes";
    const std::array names{"volume-bounded", "volume-cornell", "volume-beam"};
    const std::array<std::size_t, 3> counts{3, 8, 2};
    const std::array<double, 3> anisotropy{.6, .25, .15};
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto scene = load_scene(root / (std::string(names[i]) + ".json"));
        INFO(names[i]);
        REQUIRE(scene.objects.size() == counts[i]);
        REQUIRE(scene.media.size() == 1);
        REQUIRE(scene.media[0].medium.anisotropy == anisotropy[i]);
        REQUIRE(scene.media[0].enabled);
        const PathTracer tracer(make_snapshot(scene), {.maximum_depth = 8});
        PathStatistics statistics;
        const auto image = tracer.render({16, 16, 32, 101}, &statistics);
        REQUIRE(statistics.medium_scattering_events > 0);
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

TEST_CASE("Beam ceiling aperture preserves both open and blocked light paths",
          "[volume][presets]") {
    const auto scene =
        load_scene(std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes/volume-beam.json");
    const auto snapshot = make_snapshot(scene);
    const SceneBvh bvh(snapshot);
    const auto opening = bvh.intersect({{0, 3, 0}, {0, -1, 0}}, {0, 10});
    const auto panel = bvh.intersect({{1, 3, 0}, {0, -1, 0}}, {0, 10});
    REQUIRE(opening);
    REQUIRE(panel);
    REQUIRE(snapshot->instances[opening->instance_index].object_id == "ground");
    REQUIRE(snapshot->instances[panel->instance_index].object_id == "aperture");
    REQUIRE(std::abs(opening->position.y) < 1e-6);
    REQUIRE(std::abs(panel->position.y - 2.6) < 1e-6);
}
