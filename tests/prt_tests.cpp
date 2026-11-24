// 投影与旋转解析对照，静态遮挡和一次漫反射的颜色传递，以及缓存依赖失效。
#include <catch2/catch_test_macros.hpp>
#include <minirender/render/prt.hpp>
#include <minirender/render/rectangle_light.hpp>
#include <numbers>
using namespace minirender;

TEST_CASE("SH cubemap solid angles integrate a constant environment") {
    std::array<Image, 6> faces{Image(8, 8), Image(8, 8), Image(8, 8),
                               Image(8, 8), Image(8, 8), Image(8, 8)};
    for (auto& f : faces) {
        for (std::size_t y = 0; y < 8; ++y) {
            for (std::size_t x = 0; x < 8; ++x) {
                f.at(x, y) = {.5F, .25F, 1.F};
            }
        }
    }
    auto       sh       = project_cubemap(faces);
    const auto expected = constant_environment({.5, .25, 1});
    for (std::size_t k = 0; k < 9; ++k) {
        REQUIRE(glm::length(sh[k] - expected[k]) < 1e-10);
    }
    // 轴向旋转后 +X 的一阶瓣指向 -Z，与经纬图的环境旋转约定相同。
    ShLight l{};
    l[3]               = {1, 2, 3};
    const auto rotated = rotate_environment(l, std::numbers::pi / 2);
    REQUIRE(glm::length(rotated[2] + l[3]) < 1e-10);
    auto restored = rotate_environment(rotated, -std::numbers::pi / 2);
    for (std::size_t k = 0; k < 9; ++k) {
        REQUIRE(glm::length(restored[k] - l[k]) < 1e-9);
    }
}

TEST_CASE("Diffuse PRT includes static blockers and colored interreflection") {
    SceneDocument doc;
    doc.meshes.push_back({"quad", {}, make_emitter_quad()});
    doc.materials.push_back({"white", "white", MaterialKind::diffuse, glm::vec3(1)});
    doc.materials.push_back({"red", "red", MaterialKind::diffuse, {1, .04F, .03F}});
    doc.objects.push_back({"floor", "floor", "quad", "white", {}, true});
    Transform raised;
    raised.translation.y = .65F;
    raised.scale         = {3, 1, 3};
    doc.objects.push_back({"plate", "plate", "quad", "red", raised, true});
    auto        scene = make_snapshot(doc);
    PrtSettings s{PrtMode::unoccluded, 2048, 1, 23};
    const auto  l          = constant_environment(glm::dvec3(1));
    auto        unshadowed = bake_prt(scene, s);
    REQUIRE(unshadowed.offsets == std::vector<std::uint32_t>{0, 6});
    REQUIRE(glm::length(prt_irradiance(unshadowed.vertices.front(), l) - glm::vec3(1)) < 1e-5F);
    s.mode      = PrtMode::shadowed;
    auto direct = bake_prt(scene, s);
    auto shadow = prt_irradiance(direct.vertices.front(), l);
    REQUIRE(shadow.r < .6F);
    s.mode      = PrtMode::interreflection;
    auto bounce = bake_prt(scene, s);
    auto color  = prt_irradiance(bounce.vertices.front(), l);
    REQUIRE(color.r > shadow.r + .1F);
    REQUIRE(color.r > color.g + .1F);
    auto key        = prt_cache_key(*scene, s);
    doc.environment = {.1F, .2F, .3F};
    REQUIRE(prt_cache_key(*make_snapshot(doc), s) == key);
    doc.materials[1].base_color.g = 1.F;
    auto changed                  = make_snapshot(doc);
    REQUIRE_FALSE(compatible_prt(*scene, *changed, s.mode));
    REQUIRE(compatible_prt(*scene, *changed, PrtMode::shadowed));
    REQUIRE(prt_cache_key(*changed, s) != key);
}
