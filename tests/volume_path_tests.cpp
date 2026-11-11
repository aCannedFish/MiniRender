#include <minirender/assets/scene_io.hpp>
#include <minirender/render/path_tracer.hpp>
#include <minirender/render/render_session.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <iostream>
#include <numbers>
#include <thread>

using namespace minirender;
using Catch::Approx;
namespace {
constexpr double pi = std::numbers::pi;
const Ray horizontal{{-2, 0, 0}, {1, 0, 0}};

MediumRegion fog(glm::dvec3 absorption, glm::dvec3 scattering, double g = 0) {
    MediumRegion result;
    result.id = "fog";
    result.medium = {absorption, scattering, g};
    return result;
}
SceneDocument environment_scene() {
    SceneDocument scene;
    scene.environment = glm::vec3(1);
    scene.camera.position = {-2, 0, 0};
    scene.camera.target = {0, 0, 0};
    scene.camera.near_plane = .01F;
    scene.camera.far_plane = 100;
    return scene;
}

// 水平平面可作为接收面、有限发光面或遮挡面；尺度足够覆盖测试射线。
void add_plane(SceneDocument& scene, std::string id, float y, float half_size, Material material) {
    auto mesh = std::make_shared<MeshData>();
    mesh->positions = {{-half_size, y, -half_size},
                       {-half_size, y, half_size},
                       {half_size, y, half_size},
                       {half_size, y, -half_size}};
    mesh->triangles = {{{{{0, {0, 1, 0}, {}}, {1, {0, 1, 0}, {}}, {2, {0, 1, 0}, {}}}}},
                       {{{{0, {0, 1, 0}, {}}, {2, {0, 1, 0}, {}}, {3, {0, 1, 0}, {}}}}}};
    material.id = id;
    scene.materials.push_back(material);
    scene.meshes.push_back({id, {}, mesh});
    scene.objects.push_back({id, id, id, id, {}});
}
struct Moments {
    std::size_t count = 0;
    glm::dvec3 mean{0}, m2{0};
    void add(glm::dvec3 value) {
        ++count;
        const auto delta = value - mean;
        mean += delta / double(count);
        m2 += delta * (value - mean);
    }
    glm::dvec3 error() const {
        return glm::sqrt(m2 / double(count - 1) / double(count));
    }
};
Moments estimate(const PathTracer& tracer, Ray ray, int samples, std::uint64_t seed,
                 PathStatistics* statistics = nullptr) {
    Moments result;
    for (int i = 0; i < samples; ++i) {
        auto random = Sampler::for_sample(seed, 0, std::uint64_t(i));
        result.add(tracer.trace(ray, random, {0, 100}, statistics));
    }
    return result;
}
void check_mean(const Moments& value, glm::dvec3 expected) {
    const auto tolerance = 5.0 * value.error() + .00002;
    INFO("mean=" << value.mean.x << ',' << value.mean.y << ',' << value.mean.z << " error="
                 << value.error().x << ',' << value.error().y << ',' << value.error().z);
    for (int c = 0; c < 3; ++c) {
        REQUIRE(std::abs(value.mean[c] - expected[c]) < tolerance[c]);
    }
}
} // namespace

TEST_CASE("Volume path integrates pure absorption only within the camera clip interval",
          "[volume]") {
    auto scene = environment_scene();
    scene.media.push_back(fog({.1, .4, .8}, {}));
    const PathTracer tracer(make_snapshot(scene), {.maximum_depth = 0});
    Sampler random(77);
    const auto expected = medium_transmittance(scene.media[0].medium, 1.5);
    const auto actual = tracer.trace(horizontal, random, {1.25, 2.75});
    REQUIRE(glm::length(actual - expected) < 1e-12);
    const auto inside = tracer.trace({{0, 0, 0}, {1, 0, 0}}, random, {0, 100});
    REQUIRE(glm::length(inside - medium_transmittance(scene.media[0].medium, 1)) < 1e-12);
}

TEST_CASE("Vacuum regions preserve the existing path image and random stream", "[volume]") {
    auto scene = load_scene(std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes/studio.json");
    const PathTracer old(make_snapshot(scene));
    const auto baseline = old.render({12, 10, 8, 191});
    scene.media.push_back(fog({}, {}));
    const PathTracer vacuum(make_snapshot(scene));
    const auto current = vacuum.render({12, 10, 8, 191});
    for (std::size_t y = 0; y < 10; ++y) {
        for (std::size_t x = 0; x < 12; ++x) {
            REQUIRE(current.at(x, y) == baseline.at(x, y));
        }
    }
}

TEST_CASE("Volume single scattering matches an independent slab solution", "[volume]") {
    auto scene = environment_scene();
    scene.environment = glm::vec3(0);
    scene.directional_lights.push_back({"sun", {0, -1, 0}, {1, 1, 1}, 2});
    scene.media.push_back(fog({.1, .3, .5}, {.5, .7, .2}, .45));
    const auto& medium = scene.media[0].medium;
    const auto extinction = medium.extinction();
    // 相机在 x 方向走 0..2m；光源沿 +Y，任意首散射点的光源侧距离均为 1m。
    const double phase = (1 - .45 * .45) / (4 * pi * std::pow(1 + .45 * .45, 1.5));
    glm::dvec3 expected;
    for (int c = 0; c < 3; ++c) {
        expected[c] = 2 * phase * medium.scattering[c] / extinction[c] *
                      (-std::expm1(-2 * extinction[c])) * std::exp(-extinction[c]);
    }
    for (const auto mode : {SamplingMode::bsdf, SamplingMode::light, SamplingMode::mis}) {
        const PathTracer tracer(make_snapshot(scene), {mode, 1, false, 3});
        PathStatistics statistics;
        const auto result = estimate(tracer, horizontal, 60000, 88, &statistics);
        check_mean(result, expected);
        REQUIRE(statistics.medium_scattering_events > 30000);
        REQUIRE(statistics.shadow_rays > 30000);
    }
}

TEST_CASE("Surface next event lighting includes RGB shadow transmittance", "[volume]") {
    auto scene = environment_scene();
    scene.environment = glm::vec3(0);
    Material floor;
    floor.base_color = glm::vec3(.6F);
    add_plane(scene, "floor", 0, 4, floor);
    scene.directional_lights.push_back({"sun", {0, -1, 0}, {1, 1, 1}, 2});
    auto region = fog({.2, .5, 1}, {});
    region.minimum = {-2, 1, -2};
    region.maximum = {2, 2, 2};
    scene.media.push_back(region);
    const PathTracer tracer(make_snapshot(scene), {SamplingMode::light, 1, false, 3});
    Sampler random(17);
    const auto actual = tracer.trace({{0, .5, 0}, {0, -1, 0}}, random, {0, 10});
    const auto expected = 2 * double(.6F) / pi * medium_transmittance(region.medium, 1);
    REQUIRE(glm::length(actual - expected) < 1e-12);
    add_plane(scene, "blocker", .75F, 4, floor);
    const PathTracer blocked(make_snapshot(scene), {SamplingMode::light, 1, false, 3});
    REQUIRE(blocked.trace({{0, .5, 0}, {0, -1, 0}}, random, {0, 10}) == glm::dvec3(0));
}

TEST_CASE("Volume area light MIS matches independent surface and distance quadrature", "[volume]") {
    auto scene = environment_scene();
    scene.environment = glm::vec3(0);
    scene.media.push_back(fog(glm::dvec3(.2), glm::dvec3(.5)));
    Material lamp;
    lamp.base_color = glm::vec3(0);
    lamp.emission = glm::vec3(2);
    add_plane(scene, "lamp", 1.5F, .5F, lamp);
    // 对距离 s 和整个方形灯的 x/z 三重积分，不调用 LightSampler、相函数或介质实现。
    constexpr int n = 40;
    double expected = 0;
    const auto simpson = [](int i) { return i == 0 || i == n ? 1 : i % 2 ? 4 : 2; };
    for (int si = 0; si <= n; ++si) {
        const double s = 2.0 * si / n;
        for (int xi = 0; xi <= n; ++xi) {
            const double x = -.5 + double(xi) / n;
            for (int zi = 0; zi <= n; ++zi) {
                const double z = -.5 + double(zi) / n;
                const double squared_distance = (x + 1 - s) * (x + 1 - s) + 1.5 * 1.5 + z * z;
                const double cosine = 1.5 / std::sqrt(squared_distance);
                const double contribution = std::exp(-.7 * s) * .5 / (4 * pi) * 2 *
                                            std::exp(-.7 / cosine) * cosine / squared_distance;
                expected += simpson(si) * simpson(xi) * simpson(zi) * contribution;
            }
        }
    }
    expected *= 2.0 / (27.0 * n * n * n);
    for (const auto mode : {SamplingMode::bsdf, SamplingMode::light, SamplingMode::mis}) {
        const PathTracer tracer(make_snapshot(scene), {mode, 1, false, 3});
        const auto result = estimate(tracer, horizontal, 120000, 713);
        check_mean(result, glm::dvec3(expected));
        std::cout << "area volume mode=" << sampling_mode_name(mode) << " expected=" << expected
                  << " mean=" << result.mean.x << " stderr=" << result.error().x << '\n';
    }
}

TEST_CASE("Volume finite light shadows stop at the sampled emitter", "[volume]") {
    auto scene = environment_scene();
    scene.environment = glm::vec3(0);
    Material floor;
    floor.base_color = glm::vec3(.6F);
    add_plane(scene, "floor", 0, 4, floor);
    Material lamp;
    lamp.base_color = glm::vec3(0);
    lamp.emission = glm::vec3(2);
    add_plane(scene, "lamp", 1.5F, .4F, lamp);
    auto region = fog({.2, .5, 1}, {});
    region.minimum = {-2, 1, -2};
    region.maximum = {2, 3, 2};
    scene.media.push_back(region);
    const auto snapshot = make_snapshot(scene);
    const PathTracer tracer(snapshot, {SamplingMode::light, 1, false, 3});
    const LightSampler lights(snapshot);
    for (int i = 0; i < 128; ++i) {
        auto random = Sampler::for_sample(414, 0, std::uint64_t(i));
        auto reference = random;
        const double choice = reference.next_1d();
        const auto surface = reference.next_2d();
        const auto light = lights.sample({0, 0, 0}, choice, surface);
        REQUIRE(light);
        const auto tr = medium_transmittance(region.medium, .5 / light->direction.y);
        const auto expected =
            double(.6F) / pi * light->direction.y * light->radiance * tr / light->probability;
        const auto actual = tracer.trace({{0, .5, 0}, {0, -1, 0}}, random, {0, 10});
        REQUIRE(glm::length(actual - expected) < 1e-10);
    }
}

TEST_CASE("Medium next event lighting respects surface occlusion", "[volume]") {
    auto scene = environment_scene();
    scene.environment = glm::vec3(0);
    scene.directional_lights.push_back({"sun", {0, -1, 0}, {1, 1, 1}, 2});
    scene.media.push_back(fog({.1, .3, .5}, {.5, .7, .2}, 0));
    add_plane(scene, "blocker", .5F, 4, {});
    const PathTracer tracer(make_snapshot(scene), {SamplingMode::mis, 1, false, 3});
    const auto result = estimate(tracer, horizontal, 2000, 29);
    REQUIRE(result.mean == glm::dvec3(0));
}

TEST_CASE("Conservative RGB multiple scattering preserves uniform environment energy", "[volume]") {
    auto scene = environment_scene();
    scene.media.push_back(fog({}, {.15, .4, .8}, .55));
    SECTION("all channels scatter") {}
    SECTION("red channel remains clear") {
        scene.media[0].medium.scattering.x = 0;
    }
    for (const std::uint64_t seed : {37ULL, 947ULL, 3107ULL}) {
        for (const auto mode : {SamplingMode::bsdf, SamplingMode::light, SamplingMode::mis}) {
            PathStatistics statistics;
            const PathTracer tracer(make_snapshot(scene), {mode, 32, false, 3});
            const auto result = estimate(tracer, horizontal, 50000, seed, &statistics);
            check_mean(result, glm::dvec3(1));
            REQUIRE(statistics.medium_scattering_events > 20000);
            REQUIRE(statistics.medium_scattering_events > statistics.paths / 3);
            std::cout << "volume mode=" << sampling_mode_name(mode) << " seed=" << seed
                      << " clear_red=" << (scene.media[0].medium.scattering.x == 0)
                      << " mean=" << result.mean.x << ',' << result.mean.y << ',' << result.mean.z
                      << " stderr=" << result.error().x << ',' << result.error().y << ','
                      << result.error().z << " events=" << statistics.medium_scattering_events
                      << '\n';
        }
    }
}

TEST_CASE("Volume render sessions match serial pixels and support live cancellation",
          "[volume][session]") {
    auto scene = environment_scene();
    scene.media.push_back(fog({.1, .2, .3}, {.4, .3, .2}, .3));
    const auto snapshot = make_snapshot(scene);
    const IntegratorSettings integrator{SamplingMode::mis, 8, true, 3};
    const RenderSettings render{12, 10, 16, 101};
    const auto reference = PathTracer(snapshot, integrator).render(render);
    RenderSession session;
    session.start(snapshot, render, integrator, {2, 4, 2});
    REQUIRE(session.wait_for(std::chrono::seconds(5)));
    session.wait();
    REQUIRE(session.progress().state == RenderState::completed);
    REQUIRE(session.progress().statistics.medium_scattering_events > 0);
    const auto result = session.result();
    REQUIRE(result);
    for (std::size_t y = 0; y < render.height; ++y) {
        for (std::size_t x = 0; x < render.width; ++x) {
            REQUIRE(glm::length(result->at(x, y) - reference.at(x, y)) < 1e-6F);
        }
    }
    session.start(snapshot, {32, 32, 1000000, 111}, integrator, {2, 8, 2});
    const auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!session.progress().recorded_samples && std::chrono::steady_clock::now() < timeout) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(session.progress().recorded_samples > 0);
    session.cancel();
    REQUIRE(session.wait_for(std::chrono::seconds(2)));
    session.wait();
    REQUIRE(session.progress().state == RenderState::cancelled);
    REQUIRE(session.progress().statistics.medium_scattering_events > 0);
}
