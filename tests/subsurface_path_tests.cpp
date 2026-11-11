#include "subsurface_fixture.hpp"

#include <minirender/assets/scene_io.hpp>
#include <minirender/render/render_session.hpp>
#include <minirender/scene/editor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <numbers>
#include <thread>

using namespace minirender;
using namespace std::chrono_literals;
namespace {

struct Moments {
    std::size_t count = 0;
    glm::dvec3 mean{0}, variance_sum{0};
    void add(glm::dvec3 value) {
        ++count;
        const auto delta = value - mean;
        mean += delta / double(count);
        variance_sum += delta * (value - mean);
    }
    glm::dvec3 error() const {
        return glm::sqrt(variance_sum / double(count - 1) / double(count));
    }
};

// 唯一受方向光照射的左侧平面与相机可见的正面分开，参考直接做二维面积积分。
glm::dvec3 side_reference(const SceneDocument& scene) {
    const auto& material = scene.materials[0];
    const DipoleProfile profile(*material.subsurface, material.ior);
    const double f0 = std::pow((material.ior - 1.) / (material.ior + 1.), 2);
    glm::dvec3 integral(0);
    constexpr int count = 256;
    for (int y = 0; y < count; ++y) {
        for (int z = 0; z < count; ++z) {
            const glm::dvec3 point{-.5, (y + .5) / count - .5, (z + .5) / count - .5};
            integral +=
                profile.evaluate(glm::length(point - glm::dvec3(0, 0, .5))) / double(count * count);
        }
    }
    const auto& light = scene.directional_lights[0];
    return (1 - f0) * (1 - f0) / std::numbers::pi * integral * glm::dvec3(light.color) *
           double(light.intensity);
}

SceneDocument side_scene() {
    auto scene = sss_test::scene();
    scene.environment = glm::vec3(0);
    scene.directional_lights.push_back({"side", {1, 0, 0}, {1, .8F, .6F}, 4});
    return scene;
}

template <class Predicate> void await(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!predicate()) {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
}

class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        path = std::filesystem::temp_directory_path() /
               ("minirender-sss-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    std::filesystem::path path;
};

// 几何三角面的自适应面积积分，用较细的近点网格解析毫米尺度的 dipole 峰值。
// 积分不使用投影轴、径向采样或 BSSRDF 的面积 PDF。
glm::dvec3 integrate_triangle(const DipoleProfile& profile, glm::dvec3 origin,
                              const std::array<glm::dvec3, 3>& p, double tolerance, int depth = 0) {
    const auto center = (p[0] + p[1] + p[2]) / 3.;
    const double area = glm::length(glm::cross(p[1] - p[0], p[2] - p[0])) / 2;
    // 七点五次三角形求积；通过四个子三角形的同阶结果估计误差。
    const auto rule = [&](const std::array<glm::dvec3, 3>& triangle, double triangle_area) {
        glm::dvec3 sum =
            .225 *
            profile.evaluate(glm::length((triangle[0] + triangle[1] + triangle[2]) / 3. - origin));
        for (const auto parameters :
             {glm::dvec3(.059715871789770, .470142064105115, .132394152788506),
              glm::dvec3(.797426985353087, .101286507323456, .125939180544827)}) {
            for (std::size_t c = 0; c < 3; ++c) {
                const auto point = parameters.x * triangle[c] +
                                   parameters.y * triangle[(c + 1) % 3] +
                                   parameters.y * triangle[(c + 2) % 3];
                sum += parameters.z * profile.evaluate(glm::length(point - origin));
            }
        }
        return triangle_area * sum;
    };
    const auto coarse = rule(p, area);
    const auto ab = (p[0] + p[1]) / 2., bc = (p[1] + p[2]) / 2., ca = (p[2] + p[0]) / 2.;
    const std::array<std::array<glm::dvec3, 3>, 4> children{
        {{p[0], ab, ca}, {ab, p[1], bc}, {ca, bc, p[2]}, {ab, bc, ca}}};
    glm::dvec3 fine(0);
    for (const auto& child : children) {
        fine += rule(child, area / 4);
    }
    const double extent = std::max(
        {glm::length(p[0] - center), glm::length(p[1] - center), glm::length(p[2] - center)});
    const bool near_peak = glm::length(center - origin) < 3 * extent && extent > .00005;
    const auto error = glm::abs(fine - coarse);
    if (depth >= 14 || (!near_peak && std::max({error.x, error.y, error.z}) < tolerance)) {
        return fine;
    }
    glm::dvec3 result(0);
    for (const auto& child : children) {
        result += integrate_triangle(profile, origin, child, tolerance / 4, depth + 1);
    }
    return result;
}
} // namespace

TEST_CASE("Subsurface path transports light across positions with all sampling modes", "[sss]") {
    const auto scene = side_scene();
    const auto expected = side_reference(scene);
    const auto snapshot = make_snapshot(scene);
    for (const auto mode : {SamplingMode::bsdf, SamplingMode::light, SamplingMode::mis}) {
        const PathTracer tracer(snapshot, {mode, 1, false, 3});
        Moments result;
        PathStatistics statistics;
        for (const std::uint64_t seed : {41ULL, 71ULL, 111ULL}) {
            for (std::uint64_t i = 0; i < 10000; ++i) {
                auto random = Sampler::for_sample(seed, 0, i);
                result.add(tracer.trace({{0, 0, 3}, {0, 0, -1}}, random, {0, 10}, &statistics));
            }
        }
        std::cout << "SSS side mode " << sampling_mode_name(mode) << " expected " << expected.x
                  << ',' << expected.y << ',' << expected.z << " measured " << result.mean.x << ','
                  << result.mean.y << ',' << result.mean.z << " stderr " << result.error().x
                  << '\n';
        for (int c = 0; c < 3; ++c) {
            REQUIRE(std::abs(result.mean[c] - expected[c]) < 6 * result.error()[c] + .00005);
        }
        REQUIRE(statistics.subsurface_events > 1000);
        REQUIRE(statistics.subsurface_distance / double(statistics.subsurface_events) > .1);
        REQUIRE(statistics.subsurface_events <= statistics.scattering_events);
    }
    const PathTracer no_scattering(snapshot, {.maximum_depth = 0});
    Sampler random(1);
    REQUIRE(no_scattering.trace({{0, 0, 3}, {0, 0, -1}}, random) == glm::dvec3(0));
}

// 凸体的外向半球都看见同一个环境，方向积分可独立化为 Fresnel 半球矩。
TEST_CASE("Subsurface continuous environment MIS matches the full surface integral", "[sss]") {
    const auto scene = sss_test::scene();
    const auto snapshot = make_snapshot(scene);
    const auto& object = snapshot->instances[0];
    const DipoleProfile profile(*object.material.subsurface, object.material.ior);
    glm::dvec3 integral(0);
    constexpr int count = 128;
    for (const auto& face : object.mesh->triangles) {
        const auto p = sss_test::world_triangle(object, face);
        const double area = glm::length(glm::cross(p[1] - p[0], p[2] - p[0])) / 2;
        for (int i = 0; i < count; ++i) {
            const double root = std::sqrt((i + .5) / count);
            for (int j = 0; j < count; ++j) {
                const double v = (j + .5) / count;
                const auto point = (1 - root) * p[0] + root * (1 - v) * p[1] + root * v * p[2];
                integral += profile.evaluate(glm::length(point - glm::dvec3(0, 0, .5))) *
                            (area / (count * count));
            }
        }
    }
    const double f0 = std::pow((object.material.ior - 1.) / (object.material.ior + 1.), 2);
    const auto expected = f0 + (1 - f0) * profile.external_transmission() * integral;
    for (const auto mode : {SamplingMode::bsdf, SamplingMode::light, SamplingMode::mis}) {
        const PathTracer tracer(snapshot, {mode, 1, false, 3});
        Moments result;
        for (std::uint64_t i = 0; i < 40000; ++i) {
            auto random = Sampler::for_sample(181, 0, i);
            result.add(tracer.trace({{0, 0, 3}, {0, 0, -1}}, random));
        }
        std::cout << "SSS environment " << sampling_mode_name(mode) << " expected " << expected.x
                  << " measured " << result.mean.x << " stderr " << result.error().x << '\n';
        for (int c = 0; c < 3; ++c) {
            REQUIRE(std::abs(result.mean[c] - expected[c]) < 6 * result.error()[c] + .0002);
        }
    }
}

TEST_CASE("Measured marble on a small curved mesh matches independent area integration", "[sss]") {
    auto scene =
        load_scene(std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes/cornell-materials.json");
    std::erase_if(scene.objects, [](const auto& object) { return object.id != "glass_ball"; });
    auto& object = scene.objects[0];
    object.transform.translation = {};
    object.transform.scale = glm::vec3(.02F);
    for (auto& material : scene.materials) {
        if (material.id == object.material_id) {
            material.kind = MaterialKind::subsurface;
            material.subsurface = make_subsurface_preset("marble").parameters;
        }
    }
    scene.environment = glm::vec3(1);
    const auto snapshot = make_snapshot(scene);
    const SceneBvh bvh(snapshot);
    const auto hit = bvh.intersect({{0, 0, .2}, {0, 0, -1}});
    REQUIRE(hit);
    const auto& instance = snapshot->instances[0];
    const DipoleProfile profile(*instance.material.subsurface, instance.material.ior);
    glm::dvec3 integral(0);
    for (const auto& face : instance.mesh->triangles) {
        integral +=
            integrate_triangle(profile, hit->position, sss_test::world_triangle(instance, face),
                               1e-7 / double(instance.mesh->triangles.size()));
    }
    const double cosine = glm::normalize(glm::dvec3(hit->geometric_normal)).z;
    const double reflection = fresnel_dielectric(cosine, 1, instance.material.ior);
    const auto expected =
        reflection + (1 - reflection) * profile.external_transmission() * integral;
    const PathTracer tracer(snapshot, {SamplingMode::mis, 1, false, 3});
    Moments result;
    for (std::uint64_t i = 0; i < 40000; ++i) {
        auto random = Sampler::for_sample(201, 0, i);
        result.add(tracer.trace({{0, 0, .2}, {0, 0, -1}}, random));
    }
    std::cout << "SSS marble sphere integral " << integral.x << ',' << integral.y << ','
              << integral.z << " expected " << expected.x << ',' << expected.y << ',' << expected.z
              << " measured " << result.mean.x << ',' << result.mean.y << ',' << result.mean.z
              << " stderr " << result.error().x << '\n';
    for (int c = 0; c < 3; ++c) {
        REQUIRE(std::abs(result.mean[c] - expected[c]) < 6 * result.error()[c] + 1e-5);
    }
}

TEST_CASE("Subsurface shadow rays retain occlusion and external medium attenuation", "[sss]") {
    auto scene = side_scene();
    const auto base = side_reference(scene);
    MediumRegion medium;
    medium.id = "absorber";
    medium.minimum = {-1.5, -2, -2};
    medium.maximum = {-.6, 2, 2};
    medium.medium.absorption = {.1, .2, .3};
    scene.media.push_back(medium);
    const auto expected = base * glm::exp(-.9 * medium.medium.absorption);
    const PathTracer tracer(make_snapshot(scene), {SamplingMode::mis, 1, false, 3});
    Moments result;
    for (std::uint64_t i = 0; i < 30000; ++i) {
        auto random = Sampler::for_sample(131, 0, i);
        result.add(tracer.trace({{0, 0, 3}, {0, 0, -1}}, random));
    }
    for (int c = 0; c < 3; ++c) {
        REQUIRE(std::abs(result.mean[c] - expected[c]) < 6 * result.error()[c] + .00005);
    }
    Material blocker;
    blocker.id = "blocker";
    scene.materials.push_back(blocker);
    scene.objects.push_back(
        {"blocker", "Blocker", "mesh", "blocker", {{-1, 0, 0}, {}, {.1F, 3, 3}}});
    const PathTracer blocked(make_snapshot(scene), {SamplingMode::mis, 1, false, 3});
    for (std::uint64_t i = 0; i < 500; ++i) {
        auto random = Sampler::for_sample(131, 0, i);
        REQUIRE(blocked.trace({{0, 0, 3}, {0, 0, -1}}, random) == glm::dvec3(0));
    }
}

TEST_CASE("Subsurface boundary reflection survives when diffusion has zero scattering", "[sss]") {
    auto scene = sss_test::scene();
    scene.materials[0].subsurface =
        std::make_shared<const SubsurfaceParameters>(SubsurfaceParameters{{1, 1, 1}, {0, 0, 0}, 1});
    scene.materials[0].double_sided = false;
    const PathTracer tracer(make_snapshot(scene), {SamplingMode::mis, 1, false, 3});
    const double eta = scene.materials[0].ior;
    const double expected = std::pow((eta - 1) / (eta + 1), 2);
    Moments result;
    for (std::uint64_t i = 0; i < 50000; ++i) {
        auto random = Sampler::for_sample(151, 0, i);
        result.add(tracer.trace({{0, 0, 3}, {0, 0, -1}}, random));
    }
    REQUIRE(std::abs(result.mean.x - expected) < 6 * result.error().x);
    REQUIRE(result.mean.x == result.mean.y);
    REQUIRE_THROWS([&] {
        Sampler random(0);
        tracer.trace({{0, 0, 0}, {0, 0, 1}}, random);
    }());
    IntegratorSettings sppm;
    sppm.kind = IntegratorKind::sppm;
    REQUIRE_THROWS(SppmIntegrator(make_snapshot(scene), {4, 4, 1, 1}, sppm));
}

TEST_CASE("Subsurface parameters preserve snapshots history and UTF8 scene files", "[sss]") {
    auto scene = side_scene();
    auto precise = *scene.materials[0].subsurface;
    precise.absorption.x = .12345678901234567;
    precise.reduced_scattering.y = 2.345678901234567;
    precise.density = 1.2345678901234567;
    scene.materials[0].subsurface = std::make_shared<const SubsurfaceParameters>(precise);
    SceneEditor editor(scene);
    const auto old = editor.snapshot();
    editor.edit([](SceneDocument& document) {
        auto parameters = *document.materials[0].subsurface;
        parameters.density = 3;
        document.materials[0].subsurface = std::make_shared<const SubsurfaceParameters>(parameters);
    });
    REQUIRE(old->instances[0].material.subsurface->density == precise.density);
    REQUIRE(editor.document().materials[0].subsurface->density == 3);
    REQUIRE(editor.undo());
    REQUIRE(*editor.document().materials[0].subsurface == precise);
    REQUIRE(editor.redo());
    REQUIRE(editor.document().materials[0].subsurface->density == 3);
    REQUIRE_THROWS(editor.edit([](SceneDocument& document) {
        document.materials[0].subsurface = std::make_shared<const SubsurfaceParameters>(
            SubsurfaceParameters{{-1, 0, 0}, {1, 1, 1}, 1});
    }));
    REQUIRE(editor.document().materials[0].subsurface->density == 3);
    const TemporaryDirectory temporary;
    const auto file = temporary.path / std::filesystem::path(u8"次表面 场景.json");
    save_scene(scene, file);
    const auto reopened = load_scene(file);
    REQUIRE(*reopened.materials[0].subsurface == precise);
    const auto first = PathTracer(old, {.maximum_depth = 2}).render({8, 8, 8, 171});
    const auto second =
        PathTracer(make_snapshot(reopened), {.maximum_depth = 2}).render({8, 8, 8, 171});
    for (std::size_t y = 0; y < 8; ++y) {
        for (std::size_t x = 0; x < 8; ++x) {
            REQUIRE(first.at(x, y) == second.at(x, y));
        }
    }
}

TEST_CASE("Subsurface render sessions match serial samples and pause without blocking", "[sss]") {
    const auto snapshot = make_snapshot(sss_test::scene());
    const IntegratorSettings integrator{SamplingMode::mis, 2, false, 3};
    const RenderSettings settings{12, 8, 8, 191};
    const auto expected = PathTracer(snapshot, integrator).render(settings);
    RenderSession task;
    task.start(snapshot, settings, integrator, {3, 4, 2});
    REQUIRE(task.wait_for(10s));
    task.wait();
    REQUIRE(task.progress().state == RenderState::completed);
    REQUIRE(task.progress().statistics.subsurface_events > 0);
    const auto actual = task.result();
    REQUIRE(actual);
    for (std::size_t y = 0; y < settings.height; ++y) {
        for (std::size_t x = 0; x < settings.width; ++x) {
            REQUIRE(actual->at(x, y) == expected.at(x, y));
        }
    }
    task.start(snapshot, {32, 24, 1000000, 191}, integrator, {2, 8, 1});
    await([&] { return task.progress().recorded_samples > 0; });
    REQUIRE(task.pause());
    await([&] { return task.progress().state == RenderState::paused; });
    const auto samples = task.progress().recorded_samples;
    std::this_thread::sleep_for(20ms);
    REQUIRE(task.progress().recorded_samples == samples);
    REQUIRE(task.resume());
    await([&] { return task.progress().recorded_samples > samples; });
    const auto before = std::chrono::steady_clock::now();
    task.cancel();
    REQUIRE(task.wait_for(2s));
    task.wait();
    REQUIRE(std::chrono::steady_clock::now() - before < 2s);
    REQUIRE(task.progress().state == RenderState::cancelled);
    REQUIRE(task.preview().image);
}
