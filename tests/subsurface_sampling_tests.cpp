#include "subsurface_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>

using namespace minirender;
namespace {
using namespace sss_test;

// 独立遍历所有原始三角面，不使用 BVH 的实例过滤或 BSSRDF 的求交循环。
std::size_t brute_count(const SceneSnapshot& scene, const SurfaceHit& from, const SurfaceHit& to,
                        glm::dvec3 axis, double radius) {
    const auto delta = to.position - from.position;
    const auto offset = delta - glm::dot(delta, axis) * axis;
    const RayQuery query({from.position + offset - radius * axis, axis});
    std::vector<double> distances;
    const auto& object = scene.instances[from.instance_index];
    for (const auto& face : object.mesh->triangles) {
        const auto points = world_triangle(object, face);
        const auto hit = query.intersect(
            {glm::vec3(points[0]), glm::vec3(points[1]), glm::vec3(points[2])}, {0, 2 * radius});
        if (hit) {
            distances.push_back(hit->distance);
        }
    }
    std::sort(distances.begin(), distances.end());
    distances.erase(std::unique(distances.begin(), distances.end(),
                                [](double a, double b) { return std::abs(a - b) < 1e-10; }),
                    distances.end());
    return distances.size();
}

double brute_pdf(const SceneSnapshot& scene, const SubsurfaceSampler& sampler,
                 const SurfaceHit& from, const SurfaceHit& to, bool& differing_counts) {
    const ShadingFrame frame(glm::dvec3(from.geometric_normal));
    const std::array basis{frame.to_world({1, 0, 0}), frame.to_world({0, 1, 0}), frame.normal()};
    const std::array probabilities{.25, .25, .5};
    const auto& profile = sampler.profile(from.instance_index);
    const double bound = sampler.maximum_radius(from.instance_index);
    const auto mass = profile.cumulative(bound);
    const double total = mass.x + mass.y + mass.z;
    const auto delta = to.position - from.position;
    const auto normal = glm::normalize(glm::dvec3(to.geometric_normal));
    double density = 0;
    std::size_t previous_count = 0;
    for (std::size_t a = 0; a < 3; ++a) {
        const double cosine = std::abs(glm::dot(normal, basis[a]));
        if (cosine < 1e-12) {
            continue;
        }
        const auto count = brute_count(scene, from, to, basis[a], bound);
        if (previous_count && count != previous_count) {
            differing_counts = true;
        }
        previous_count = count;
        const double radius = glm::length(delta - glm::dot(delta, basis[a]) * basis[a]);
        const auto value = profile.evaluate(radius);
        density +=
            probabilities[a] * (value.x + value.y + value.z) / total * cosine / double(count);
    }
    return density;
}

glm::dvec3 spatial_function(glm::dvec3 point) {
    return {1 + .1 * point.x, .8 + .07 * point.y, .6 + .05 * point.z};
}

// 按每个世界三角面的真实面积积分，完全独立于投影采样和面积 PDF。
glm::dvec3 area_integral(const SceneSnapshot& scene, const SurfaceHit& from,
                         const DipoleProfile& profile, int count) {
    glm::dvec3 result(0);
    const auto& object = scene.instances[from.instance_index];
    for (const auto& face : object.mesh->triangles) {
        const auto p = world_triangle(object, face);
        const double area = glm::length(glm::cross(p[1] - p[0], p[2] - p[0])) / 2;
        for (int i = 0; i < count; ++i) {
            const double root = std::sqrt((i + .5) / count);
            for (int j = 0; j < count; ++j) {
                const double v = (j + .5) / count;
                const auto point = (1 - root) * p[0] + root * (1 - v) * p[1] + root * v * p[2];
                result += profile.evaluate(glm::length(point - from.position)) *
                          spatial_function(point) * (area / (count * count));
            }
        }
    }
    return result;
}
} // namespace

TEST_CASE("Subsurface probes include both boundaries and isolate the selected instance", "[sss]") {
    auto document = sss_test::scene();
    document.materials[0].double_sided = false;
    auto other = document.objects[0];
    other.id = "other";
    other.transform.translation = {0, 0, 2};
    document.objects.push_back(other);
    const auto snapshot = make_snapshot(document);
    const SceneBvh bvh(snapshot);
    const Ray ray{{0, 0, 5}, {0, 0, -1}};
    const auto first = bvh.intersect_instance(ray, 0);
    REQUIRE(first);
    REQUIRE(first->position.z == .5);
    const auto back = bvh.intersect_instance(ray, 0, {5, 6});
    REQUIRE(back);
    REQUIRE(back->position.z == -.5);
    REQUIRE_FALSE(back->front_face);
    REQUIRE_THROWS(bvh.intersect_instance(ray, 2));
    const SubsurfaceSampler sampler(bvh);
    Sampler random(121);
    bool differing = false;
    for (int i = 0; i < 1000; ++i) {
        const auto sample = sampler.sample(*first, random.next_1d(), random.next_1d(),
                                           random.next_2d(), random.next_1d());
        if (sample) {
            REQUIRE(sample->surface.instance_index == 0);
            REQUIRE(std::abs(sample->probability /
                                 brute_pdf(*snapshot, sampler, *first, sample->surface, differing) -
                             1) < 1e-10);
        }
    }
    REQUIRE(sampler.pdf(*first, *bvh.intersect_instance(ray, 1)) == 0);
}

TEST_CASE("Concave subsurface PDFs use each projection axis intersection count", "[sss]") {
    auto document = sss_test::scene(true);
    document.objects[0].transform.rotation_degrees = {17, 23, 31};
    document.objects[0].transform.scale = {-1.3F, .8F, .7F};
    document.objects[0].transform.translation = {.3F, -.2F, .4F};
    const auto snapshot = make_snapshot(document);
    const SceneBvh bvh(snapshot);
    const SubsurfaceSampler sampler(bvh);
    const auto from = sss_test::entry(bvh, {-1, .5, 1});
    Sampler random(371);
    bool differing = false;
    int accepted = 0;
    for (int i = 0; i < 3000; ++i) {
        const double axis = random.next_1d(), channel = random.next_1d();
        const auto disk = random.next_2d();
        const auto sample = sampler.sample(from, axis, channel, disk, random.next_1d());
        if (!sample) {
            continue;
        }
        ++accepted;
        const double expected = brute_pdf(*snapshot, sampler, from, sample->surface, differing);
        REQUIRE(std::abs(sample->probability / expected - 1) < 1e-8);
        REQUIRE(std::abs(sampler.pdf(from, sample->surface) / expected - 1) < 1e-8);
    }
    REQUIRE(accepted > 500);
    REQUIRE(differing);
}

TEST_CASE("Subsurface surface estimator matches independent world area quadrature", "[sss]") {
    const auto snapshot = make_snapshot(sss_test::scene(true));
    const SceneBvh bvh(snapshot);
    const SubsurfaceSampler sampler(bvh);
    const auto from = sss_test::entry(bvh, {-1, .5, 1});
    const auto coarse = area_integral(*snapshot, from, sampler.profile(0), 64);
    const auto expected = area_integral(*snapshot, from, sampler.profile(0), 128);
    REQUIRE(glm::length(coarse - expected) < .0002);
    glm::dvec3 sum(0), squares(0);
    constexpr int count = 50000;
    for (int i = 0; i < count; ++i) {
        auto random = Sampler::for_sample(177, 0, std::uint64_t(i));
        const double axis = random.next_1d(), channel = random.next_1d();
        const auto disk = random.next_2d();
        const auto sample = sampler.sample(from, axis, channel, disk, random.next_1d());
        const auto value =
            sample ? sample->weight * spatial_function(sample->surface.position) : glm::dvec3(0);
        sum += value;
        squares += value * value;
    }
    const auto mean = sum / double(count);
    const auto error =
        glm::sqrt(glm::max(glm::dvec3(0), squares / double(count) - mean * mean) / double(count));
    std::cout << "SSS area expected " << expected.x << ',' << expected.y << ',' << expected.z
              << " measured " << mean.x << ',' << mean.y << ',' << mean.z << " stderr " << error.x
              << ',' << error.y << ',' << error.z << '\n';
    for (int c = 0; c < 3; ++c) {
        REQUIRE(std::abs(mean[c] - expected[c]) < 6 * error[c] + .0002);
    }
}

TEST_CASE("Subsurface requires a closed connected outward mesh and supports cancellation",
          "[sss]") {
    auto document = sss_test::scene();
    auto mesh = std::make_shared<MeshData>(*document.meshes[0].data);
    mesh->triangles.pop_back();
    document.meshes[0].data = mesh;
    const SceneBvh open(make_snapshot(document));
    REQUIRE_THROWS(SubsurfaceSampler(open));
    mesh = sss_test::voxel_mesh({{0, 0, 0}});
    for (auto& face : mesh->triangles) {
        std::swap(face.corners[1], face.corners[2]);
    }
    document.meshes[0].data = mesh;
    const SceneBvh inward(make_snapshot(document));
    REQUIRE_THROWS(SubsurfaceSampler(inward));
    document.meshes[0].data = sss_test::voxel_mesh({{0, 0, 0}, {2, 0, 0}});
    const SceneBvh disconnected(make_snapshot(document));
    REQUIRE_THROWS(SubsurfaceSampler(disconnected));
    document = sss_test::scene();
    const SceneBvh valid(make_snapshot(document));
    const SubsurfaceSampler sampler(valid);
    CancellationSource cancel;
    cancel.request();
    REQUIRE_THROWS_AS(SubsurfaceSampler(valid, cancel.token()), OperationCancelled);
    const auto from = sss_test::entry(valid);
    REQUIRE_THROWS_AS(sampler.sample(from, 0, 0, {.5, .5}, 0, nullptr, cancel.token()),
                      OperationCancelled);
    REQUIRE_THROWS(sampler.sample(from, 1, 0, {.5, .5}, 0));
}
