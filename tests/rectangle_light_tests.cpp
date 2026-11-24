// 数值面积积分作为矩形光源的参考，检查近场、背面和掠射角下的球面积分。
#include <catch2/catch_test_macros.hpp>
#include <minirender/render/bsdf.hpp>
#include <minirender/render/rectangle_light.hpp>
#include <numbers>

using namespace minirender;

namespace {
RectangleLight ceiling() {
    return {{{{-.6F, 2, -.4F}, {.6F, 2, -.4F}, {.6F, 2, .4F}, {-.6F, 2, .4F}}},
            glm::vec3(9),
            false,
            "lamp"};
}

// 在发光面均匀划分 n×n 个小矩形，手动做面积到立体角的变换。
glm::dvec3 quadrature(const RectangleLight& lamp, const Material& material, glm::dvec3 point,
                      glm::dvec3 view) {
    const auto         u      = glm::dvec3(lamp.corners[1] - lamp.corners[0]),
                       v      = glm::dvec3(lamp.corners[3] - lamp.corners[0]);
    const auto         normal = glm::normalize(glm::cross(u, v));
    const double       area   = glm::length(glm::cross(u, v));
    const ShadingFrame frame({0, 1, 0});
    const Bsdf         bsdf(material);
    glm::dvec3         total(0);
    constexpr int      n = 160;
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const auto delta =
                glm::dvec3(lamp.corners[0]) + u * ((x + .5) / n) + v * ((y + .5) / n) - point;
            const auto   direction = glm::normalize(delta);
            const double cosine    = std::max(0., glm::dot(normal, -direction));
            const double weight =
                area * cosine * std::max(0., direction.y) / (glm::dot(delta, delta) * n * n);
            total += glm::dvec3(lamp.radiance) *
                     bsdf.evaluate(frame.to_local(view), frame.to_local(direction)) * weight;
        }
    }
    return total;
}
} // namespace

TEST_CASE("Rectangle edge integral matches direct diffuse area quadrature") {
    const auto       lamp = ceiling();
    Material         m{"m", "m", MaterialKind::diffuse, {.8F, .4F, .2F}};
    const glm::dvec3 view{0, 1, 0};
    for (glm::dvec3 point : {glm::dvec3(0), glm::dvec3(.8, 0, -.4), glm::dvec3(.1, 1.7, .1)}) {
        const auto output = evaluate_rectangle(lamp, point, view, view, m.base_color, .5, false);
        REQUIRE(glm::length(output.diffuse - quadrature(lamp, m, point, view)) < .0003);
    }
    const auto back =
        evaluate_rectangle(lamp, {0, 3, 0}, {0, -1, 0}, {0, -1, 0}, m.base_color, .5, false);
    REQUIRE(glm::length(back.diffuse) == 0);
}

TEST_CASE("GGX LTC follows a rough conductor's independent numerical area integral") {
    auto             lamp = ceiling();
    Material         m{"gold", "gold", MaterialKind::conductor, {.91F, .70F, .30F}};
    const glm::dvec3 point{.18, 0, 0};
    for (float roughness : {.4F, .7F, .95F}) {
        m.roughness          = roughness;
        const auto view      = glm::normalize(glm::dvec3(.2, 1, .15));
        const auto reference = quadrature(lamp, m, point, view);
        const auto value =
            evaluate_rectangle(lamp, point, {0, 1, 0}, view, m.base_color, roughness, true);
        CAPTURE(roughness, value.specular.x, reference.x);
        REQUIRE(glm::length(value.specular - reference) / glm::length(reference) < .16);
    }
}

TEST_CASE("An emissive triangle pair provides one editable rectangular light") {
    SceneDocument scene;
    auto          mesh = make_emitter_quad();
    scene.meshes.push_back({"quad", {}, mesh});
    Material emitter{"emit", "L"};
    emitter.emission     = {8, 6, 2};
    emitter.double_sided = false;
    scene.materials.push_back(emitter);
    scene.objects.push_back({"light", "light", "quad", "emit", {}, true});
    auto first = make_snapshot(scene);
    REQUIRE(first->rectangles.size() == 1);
    REQUIRE(first->instances.size() == 1);
    scene.objects[0].transform.translation = {1, 2, 3};
    scene.objects[0].transform.scale       = {-2, 1, 3};
    auto        moved                      = make_snapshot(scene);
    const auto& light                      = moved->rectangles.front();
    const auto  normal =
        glm::cross(light.corners[1] - light.corners[0], light.corners[3] - light.corners[0]);
    REQUIRE(normal.y > 0);
    REQUIRE(std::abs(glm::length(normal) - 6.F) < 1e-5F);
    REQUIRE(light.radiance == emitter.emission);
    scene.objects[0].visible = false;
    REQUIRE(make_snapshot(scene)->rectangles.empty());
}
