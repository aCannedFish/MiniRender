// 切线空间法线在 CPU 求交中的解码，验证线性色彩编码、UV 方向与关闭贴图。
#include <catch2/catch_test_macros.hpp>
#include <minirender/render/bvh.hpp>
#include <minirender/render/rectangle_light.hpp>

using namespace minirender;

TEST_CASE("Tangent-space texture perturbs shading normal and retains geometric sidedness") {
    SceneDocument scene;
    scene.meshes.push_back({"quad", {}, make_emitter_quad()});
    Image encoded(1, 1);
    encoded.at(0, 0) = {.5F, .8F, .9F};
    scene.textures.push_back({"normal", "normal.png", ColorEncoding::linear,
                              std::make_shared<Texture>(std::move(encoded))});
    Material m{"m", "m"};
    m.normal_texture = "normal";
    scene.materials.push_back(m);
    scene.objects.push_back({"quad", "quad", "quad", "m", {}, true});
    const auto hit = [&] {
        SceneBvh   bvh(make_snapshot(scene));
        const auto result = bvh.intersect({{.05, 1, 0}, {0, -1, 0}});
        REQUIRE(result.has_value());
        REQUIRE(result->geometric_normal == glm::vec3(0, 1, 0));
        return result->shading_normal;
    };
    REQUIRE(glm::length(hit() - glm::vec3(0, .8F, .6F)) < 1e-4F);
    scene.materials[0].normal_flip_y = true;
    REQUIRE(glm::length(hit() - glm::vec3(0, .8F, -.6F)) < 1e-4F);
    scene.materials[0].normal_scale = 0;
    REQUIRE(hit() == glm::vec3(0, 1, 0));
    scene.textures[0].encoding = ColorEncoding::srgb;
    REQUIRE_THROWS(scene.validate());
}
