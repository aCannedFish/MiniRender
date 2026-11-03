#include <minirender/assets/scene_io.hpp>
#include <minirender/render/bvh.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <future>
#include <stdexcept>

using namespace minirender;
using Catch::Approx;

namespace {

std::shared_ptr<const MeshData> triangle_mesh() {
    MeshData mesh;
    mesh.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    mesh.triangles.push_back(
        {{{{0, {0, 0, 1}, {0, 0}}, {1, {0, 0, 1}, {1, 0}}, {2, {0, 0, 1}, {0, 1}}}}});
    mesh.validate();
    return std::make_shared<const MeshData>(std::move(mesh));
}

SceneDocument triangle_scene() {
    SceneDocument document;
    document.materials.push_back({"mat", "mat"});
    document.meshes.push_back({"triangle", {}, triangle_mesh()});
    document.objects.push_back({"triangle", "triangle", "triangle", "mat", {}, true});
    return document;
}

// 独立参考使用世界空间 Möller–Trumbore，避免与生产的置换/剪切算法共用边函数。
// 随机射线用于比较一般位置；精确边界由另一个解析测试覆盖。
struct ReferenceTriangle {
    std::array<glm::dvec3, 3> vertices;
    std::uint32_t instance;
    std::uint32_t triangle;
};

std::vector<ReferenceTriangle> reference_triangles(const SceneSnapshot& scene) {
    std::vector<ReferenceTriangle> result;
    for (std::size_t index = 0; index < scene.instances.size(); ++index) {
        const auto& instance = scene.instances[index];
        for (std::size_t face = 0; face < instance.mesh->triangles.size(); ++face) {
            ReferenceTriangle triangle{};
            triangle.instance = static_cast<std::uint32_t>(index);
            triangle.triangle = static_cast<std::uint32_t>(face);
            for (std::size_t corner = 0; corner < 3; ++corner) {
                const auto position =
                    instance.mesh
                        ->positions[instance.mesh->triangles[face].corners[corner].position];
                triangle.vertices[corner] =
                    glm::vec3(instance.object_to_world * glm::vec4(position, 1));
            }
            result.push_back(triangle);
        }
    }
    return result;
}

struct ReferenceHit {
    double distance;
    std::uint32_t instance;
    std::uint32_t triangle;
};

std::optional<ReferenceHit> brute_force(const std::vector<ReferenceTriangle>& triangles,
                                        const Ray& ray, RayInterval interval) {
    std::optional<ReferenceHit> result;
    for (const auto& triangle : triangles) {
        const auto edge1 = triangle.vertices[1] - triangle.vertices[0];
        const auto edge2 = triangle.vertices[2] - triangle.vertices[0];
        const auto p = glm::cross(glm::dvec3(ray.direction), edge2);
        const double determinant = glm::dot(edge1, p);
        if (std::abs(determinant) < 1e-14) {
            continue;
        }
        const auto offset = glm::dvec3(ray.origin) - triangle.vertices[0];
        const double u = glm::dot(offset, p) / determinant;
        const auto q = glm::cross(offset, edge1);
        const double v = glm::dot(glm::dvec3(ray.direction), q) / determinant;
        const double t = glm::dot(edge2, q) / determinant;
        if (u >= 0 && v >= 0 && u + v <= 1 && t >= interval.minimum && t <= interval.maximum) {
            if (!result || t < result->distance) {
                result = {t, triangle.instance, triangle.triangle};
                interval.maximum = t;
            }
        }
    }
    return result;
}

// 测试 RNG 固定整数递推，不依赖标准库 uniform_real_distribution 的平台实现。
float random_signed(std::uint32_t& state) {
    state = state * 1664525U + 1013904223U;
    return static_cast<float>(state >> 8U) / 8388608.0F - 1;
}

} // namespace

TEST_CASE("Ray intervals and queries reject invalid distance conventions", "[intersection]") {
    REQUIRE_THROWS_AS(RayQuery(Ray{{0, 0, 0}, {0, 0, 0}}), std::invalid_argument);
    REQUIRE_THROWS_AS(RayQuery(Ray{{0, 0, 0}, {0, 0, -2}}), std::invalid_argument);
    REQUIRE_THROWS_AS(RayQuery(Ray{{std::numeric_limits<float>::infinity(), 0, 0}, {0, 0, 1}}),
                      std::invalid_argument);
    REQUIRE_THROWS_AS((RayInterval{-1, 2}.validate()), std::invalid_argument);
    REQUIRE_THROWS_AS((RayInterval{2, 1}.validate()), std::invalid_argument);
    REQUIRE_THROWS_AS((RayInterval{0, std::numeric_limits<double>::quiet_NaN()}.validate()),
                      std::invalid_argument);
    REQUIRE_NOTHROW((RayInterval{0, std::numeric_limits<double>::infinity()}.validate()));
}

TEST_CASE("Slab queries cover parallel rays, boundary origins and finite segments",
          "[intersection]") {
    Bounds3 box;
    REQUIRE(box.empty());
    REQUIRE(box.surface_area() == 0);
    box.expand(glm::vec3(-1));
    box.expand(glm::vec3(1));
    REQUIRE(box.surface_area() == Approx(24));
    const RayQuery toward(Ray{{0, 0, 3}, {0, -0.0F, -1}});
    REQUIRE(toward.intersect(box, {0, 10}) == Approx(2));
    REQUIRE_FALSE(toward.intersect(box, {0, 1.9}));
    REQUIRE(toward.intersect(box, {2, 2}));
    REQUIRE(RayQuery(Ray{{1, 0, 3}, {0, 0, -1}}).intersect(box, {0, 10}));
    REQUIRE_FALSE(RayQuery(Ray{{1.01F, 0, 3}, {0, 0, -1}}).intersect(box, {0, 10}));
    REQUIRE_FALSE(RayQuery(Ray{{0, 0, 3}, {0, 0, 1}}).intersect(box, {0, 10}));
    REQUIRE(RayQuery(Ray{{0, 0, 0}, {1, 0, 0}}).intersect(box, {0, 10}) == Approx(0));
    REQUIRE_FALSE(toward.intersect(Bounds3{}, {0, 10}));
}

TEST_CASE("Triangle shear queries preserve barycentrics and closed distance endpoints",
          "[intersection]") {
    const std::array<glm::vec3, 3> triangle{{{0, 0, 0}, {2, 0, 0}, {0, 2, 0}}};
    const RayQuery query(Ray{{0.5F, 0.5F, 2}, {0, 0, -1}});
    const auto hit = query.intersect(triangle, {0, 10});
    REQUIRE(hit);
    REQUIRE(hit->distance == Approx(2));
    REQUIRE(hit->barycentric.x == Approx(0.5F));
    REQUIRE(hit->barycentric.y == Approx(0.25F));
    REQUIRE(hit->barycentric.z == Approx(0.25F));
    REQUIRE(query.intersect(triangle, {2, 2}));
    REQUIRE_FALSE(query.intersect(triangle, {0, 1.99}));
    REQUIRE_FALSE(query.intersect(triangle, {2.01, 10}));
    REQUIRE(RayQuery(Ray{{0.5F, 0.5F, -2}, {0, 0, 1}}).intersect(triangle, {0, 10}));
    REQUIRE_FALSE(RayQuery(Ray{{3, 3, 2}, {0, 0, -1}}).intersect(triangle, {0, 10}));
    REQUIRE_FALSE(RayQuery(Ray{{0.5F, 0.5F, 2}, {1, 0, 0}}).intersect(triangle, {0, 10}));
}

TEST_CASE("Adjacent triangles cover their shared edge and vertices across scales",
          "[intersection]") {
    for (const float scale : {0.001F, 1.0F, 128.0F}) {
        const glm::vec3 offset = scale > 1 ? glm::vec3(1e7F, -1e7F, 1e7F) : glm::vec3(0);
        const glm::vec3 a = offset;
        const glm::vec3 b = offset + glm::vec3(scale, 0, 0);
        const glm::vec3 c = offset + glm::vec3(scale, scale, 0);
        const glm::vec3 d = offset + glm::vec3(0, scale, 0);
        for (int index = 0; index <= 256; ++index) {
            const float t = static_cast<float>(index) / 256;
            const glm::vec3 point = offset + glm::vec3(t * scale, t * scale, scale * 2);
            const RayQuery query(Ray{point, {0, 0, -1}});
            const auto first = query.intersect(std::array{a, b, c}, {0, scale * 4});
            const auto second = query.intersect(std::array{a, c, d}, {0, scale * 4});
            REQUIRE((first || second));
            REQUIRE((first ? first->distance : second->distance) ==
                    Approx(scale * 2).epsilon(1e-5));
        }
    }
}

TEST_CASE("BVH interpolates surface data through nonuniform and mirrored transforms", "[bvh]") {
    auto document = triangle_scene();
    auto& transform = document.objects[0].transform;
    transform.translation = {2, -1, 3};
    transform.rotation_degrees = {15, 32, -11};
    transform.scale = {-2, 0.5F, 3};
    const auto snapshot = make_snapshot(document);
    const SceneBvh bvh(snapshot);
    const auto target = glm::vec3(transform.matrix() * glm::vec4(0.25F, 0.25F, 0, 1));
    const auto normal = glm::normalize(transform.normal_matrix() * glm::vec3(0, 0, 1));
    const auto hit = bvh.intersect({target + normal * 5.0F, -normal});
    REQUIRE(hit);
    REQUIRE(hit->distance == Approx(5).margin(1e-5));
    REQUIRE(glm::length(hit->position - glm::dvec3(target)) < 1e-5F);
    REQUIRE(glm::dot(hit->geometric_normal, normal) > 0.99999F);
    REQUIRE(glm::dot(hit->shading_normal, normal) > 0.99999F);
    REQUIRE(hit->front_face);
    REQUIRE(hit->uv.x == Approx(0.25F).margin(1e-5F));
    REQUIRE(hit->uv.y == Approx(0.25F).margin(1e-5F));
    REQUIRE(hit->instance_index == 0);
    REQUIRE(hit->triangle_index == 0);
    const auto back = bvh.intersect({target - normal * 5.0F, normal});
    REQUIRE(back);
    REQUIRE_FALSE(back->front_face);
    REQUIRE(glm::dot(back->geometric_normal, hit->geometric_normal) > 0.99999F);
    document.objects[0].transform.translation.x += 10;
    REQUIRE(bvh.intersect({target + normal * 5.0F, -normal}));
    REQUIRE(bvh.scene().instances[0].object_to_world == snapshot->instances[0].object_to_world);
    REQUIRE_FALSE(SceneBvh(make_snapshot(document)).intersect({target + normal * 5.0F, -normal}));
}

TEST_CASE("Tilted shared edges remain covered by both triangle and BVH queries",
          "[intersection][bvh]") {
    auto document = triangle_scene();
    MeshData mesh;
    const glm::vec3 origin{1e4F, -2e4F, 3e4F};
    const glm::vec3 u{2, 0.3F, -0.4F};
    const glm::vec3 v{-0.2F, 1.7F, 0.6F};
    mesh.positions = {origin, origin + u, origin + u + v, origin + v};
    const auto normal = glm::normalize(glm::cross(u, v));
    mesh.triangles.push_back({{{{0, normal, {}}, {1, normal, {}}, {2, normal, {}}}}});
    mesh.triangles.push_back({{{{0, normal, {}}, {2, normal, {}}, {3, normal, {}}}}});
    mesh.validate();
    document.meshes[0].data = std::make_shared<const MeshData>(mesh);
    const SceneBvh bvh(make_snapshot(document));
    const std::array first{mesh.positions[0], mesh.positions[1], mesh.positions[2]};
    const std::array second{mesh.positions[0], mesh.positions[2], mesh.positions[3]};
    for (int index = 1; index < 1000; ++index) {
        const float weight = static_cast<float>(index) / 1000;
        const auto point = glm::mix(mesh.positions[0], mesh.positions[2], weight);
        const Ray ray{point + normal * 3.0F, -normal};
        const RayQuery query(ray);
        REQUIRE((query.intersect(first, {0, 10}) || query.intersect(second, {0, 10})));
        REQUIRE(bvh.intersect(ray));
    }
}

TEST_CASE("Needle triangles and smooth face-corner normals retain usable hit data",
          "[intersection][bvh]") {
    const std::array<glm::vec3, 3> needle{{{0, 0, 0}, {1e6F, 0, 0}, {0, 1e-4F, 0}}};
    const auto needle_hit = RayQuery(Ray{{5e5F, 2.5e-5F, 1}, {0, 0, -1}}).intersect(needle, {0, 2});
    REQUIRE(needle_hit);
    REQUIRE(needle_hit->barycentric.x == Approx(0.25F).margin(1e-6F));
    REQUIRE(needle_hit->barycentric.y == Approx(0.5F).margin(1e-6F));
    REQUIRE(needle_hit->barycentric.z == Approx(0.25F).margin(1e-6F));
    auto document = triangle_scene();
    auto mesh = std::make_shared<MeshData>(*document.meshes[0].data);
    mesh->triangles[0].corners[1].normal = glm::normalize(glm::vec3(1, 0, 1));
    mesh->triangles[0].corners[2].normal = glm::normalize(glm::vec3(0, 1, 1));
    document.meshes[0].data = mesh;
    const SceneBvh bvh(make_snapshot(document));
    const auto hit = bvh.intersect({{0.25F, 0.25F, 2}, {0, 0, -1}});
    REQUIRE(hit);
    const auto expected =
        glm::normalize(0.5F * glm::vec3(0, 0, 1) + 0.25F * mesh->triangles[0].corners[1].normal +
                       0.25F * mesh->triangles[0].corners[2].normal);
    REQUIRE(glm::length(hit->shading_normal - expected) < 1e-6F);
    document.objects[0].transform.translation = glm::vec3(1e10F);
    document.objects[0].transform.scale = glm::vec3(1e-4F);
    REQUIRE_THROWS_AS(SceneBvh(make_snapshot(document)), std::invalid_argument);
}

TEST_CASE("BVH nearest and shadow queries agree with independent brute force", "[bvh]") {
    auto document = load_scene(std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes/studio.json");
    document.objects.clear();
    for (int z = 0; z < 5; ++z) {
        for (int x = 0; x < 6; ++x) {
            Transform transform;
            transform.translation = {static_cast<float>(x * 2 - 5), 0,
                                     static_cast<float>(z * 2 - 4)};
            transform.rotation_degrees = {static_cast<float>(x * 7), static_cast<float>(z * 11), 0};
            transform.scale = {x % 2 ? -0.7F : 0.7F, 0.6F + static_cast<float>(z) * 0.1F, 1};
            const auto id = std::to_string(x + z * 6);
            document.objects.push_back({id, id, "cube", "blue", transform, true});
        }
    }
    const auto snapshot = make_snapshot(document);
    const SceneBvh bvh(snapshot);
    const auto triangles = reference_triangles(*snapshot);
    REQUIRE(bvh.statistics().triangles == triangles.size());
    REQUIRE(bvh.statistics().nodes > 1);
    REQUIRE(bvh.statistics().maximum_depth <= 48);
    std::uint32_t random = 0x12345678U;
    std::size_t hits = 0;
    TraceStatistics statistics;
    for (int index = 0; index < 4000; ++index) {
        const glm::vec3 origin{random_signed(random) * 8, 3 + random_signed(random),
                               random_signed(random) * 8};
        const glm::vec3 target{random_signed(random) * 7, random_signed(random),
                               random_signed(random) * 7};
        const Ray ray{origin, glm::normalize(target - origin)};
        const RayInterval interval{1e-5, index % 2 ? 100.0 : 3.0};
        const auto reference = brute_force(triangles, ray, interval);
        const auto accelerated = bvh.intersect(ray, interval, &statistics);
        REQUIRE(accelerated.has_value() == reference.has_value());
        REQUIRE(bvh.occluded(ray, interval) == reference.has_value());
        if (reference) {
            ++hits;
            REQUIRE(accelerated->distance == Approx(reference->distance).margin(1e-7));
            REQUIRE(accelerated->instance_index == reference->instance);
            REQUIRE(accelerated->triangle_index == reference->triangle);
            REQUIRE(glm::length(accelerated->shading_normal) == Approx(1).margin(1e-6));
            REQUIRE(glm::dot(accelerated->shading_normal, accelerated->geometric_normal) >= 0);
        }
    }
    REQUIRE(hits > 300);
    INFO("triangle tests " << statistics.triangle_tests << " versus brute force "
                           << triangles.size() * 4000);
    REQUIRE(statistics.triangle_tests < triangles.size() * 4000);
}

TEST_CASE("Empty and coincident-centroid scenes terminate and any-hit stops early", "[bvh]") {
    REQUIRE_THROWS_AS(SceneBvh(nullptr), std::invalid_argument);
    const SceneBvh empty(make_snapshot(SceneDocument{}));
    REQUIRE(empty.statistics().nodes == 0);
    REQUIRE_FALSE(empty.intersect({}));
    REQUIRE_FALSE(empty.occluded({}));
    auto document = triangle_scene();
    for (int index = 1; index < 256; ++index) {
        const auto id = std::to_string(index);
        document.objects.push_back({id, id, "triangle", "mat", {}, true});
    }
    const SceneBvh bvh(make_snapshot(document));
    REQUIRE(bvh.statistics().nodes == 1);
    const Ray ray{{0.25F, 0.25F, 2}, {0, 0, -1}};
    TraceStatistics nearest, shadow;
    const auto hit = bvh.intersect(ray, {}, &nearest);
    REQUIRE(hit);
    REQUIRE(hit->instance_index == 0);
    REQUIRE(bvh.occluded(ray, {}, &shadow));
    REQUIRE(nearest.triangle_tests == 256);
    REQUIRE(shadow.triangle_tests == 1);
    REQUIRE_FALSE(bvh.occluded(ray, {0, 1.9}));
    REQUIRE_THROWS_AS(bvh.intersect(ray, {3, 2}), std::invalid_argument);
}

TEST_CASE("Large finite instance scales retain finite shading normals", "[bvh][transform]") {
    auto document = triangle_scene();
    document.objects[0].transform.scale = {1e15F, 2e15F, 3e15F};
    const SceneBvh bvh(make_snapshot(document));
    const auto hit = bvh.intersect({{2.5e14F, 5e14F, 3e15F}, {0, 0, -1}});
    REQUIRE(hit);
    REQUIRE(std::isfinite(hit->shading_normal.x));
    REQUIRE(std::isfinite(hit->shading_normal.y));
    REQUIRE(std::isfinite(hit->shading_normal.z));
    REQUIRE(hit->shading_normal.z == Approx(1));
}

TEST_CASE("BVH prunes separated triangles and supports concurrent const queries", "[bvh]") {
    auto document = triangle_scene();
    document.objects.clear();
    for (int x = 0; x < 64; ++x) {
        Transform transform;
        transform.translation.x = static_cast<float>(x * 2);
        const auto id = std::to_string(x);
        document.objects.push_back({id, id, "triangle", "mat", transform, true});
    }
    const SceneBvh bvh(make_snapshot(document));
    const auto query = [&bvh] {
        TraceStatistics statistics;
        for (int x = 0; x < 64; ++x) {
            const Ray ray{{static_cast<float>(x * 2) + 0.25F, 0.25F, 2}, {0, 0, -1}};
            const auto hit = bvh.intersect(ray, {}, &statistics);
            if (!hit || hit->instance_index != static_cast<std::uint32_t>(x)) {
                throw std::runtime_error("Concurrent BVH query differs");
            }
        }
        return statistics.triangle_tests;
    };
    std::vector<std::future<std::uint64_t>> futures;
    for (int worker = 0; worker < 4; ++worker) {
        futures.push_back(std::async(std::launch::async, query));
    }
    for (auto& future : futures) {
        // 该场景沿 X 分离，每条射线只经过一个叶子，叶子最多含四个三角形。
        REQUIRE(future.get() <= 64 * 4);
    }
}
