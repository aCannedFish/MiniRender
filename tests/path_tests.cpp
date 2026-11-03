#include <minirender/assets/scene_io.hpp>
#include <minirender/render/light.hpp>
#include <minirender/render/path_tracer.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <numbers>

using namespace minirender;
using Catch::Approx;

namespace {

// 单个发光三角形，用于检查有限阴影线段不会把终点光源当成遮挡物。
SceneDocument emitter_scene() {
    SceneDocument scene;
    scene.environment = glm::vec3(0);
    MeshData mesh;
    mesh.positions = {{-1, -1, 0}, {1, -1, 0}, {-1, 1, 0}};
    mesh.triangles.push_back({{{{0, {0, 0, 1}, {}}, {1, {0, 0, 1}, {}}, {2, {0, 0, 1}, {}}}}});
    scene.meshes.push_back({"mesh", {}, std::make_shared<const MeshData>(std::move(mesh))});
    scene.materials.push_back({"lamp", "lamp"});
    scene.materials[0].emission = glm::vec3(1);
    scene.objects.push_back({"lamp", "lamp", "mesh", "lamp", {}, true});
    return scene;
}

SceneDocument floor_scene(MaterialKind kind = MaterialKind::diffuse, float color = 0.6F) {
    SceneDocument scene;
    scene.environment = glm::vec3(1);
    scene.camera.position = {0, 1, 0};
    scene.camera.target = {0, 0, 0};
    scene.camera.up = {0, 0, -1};
    MeshData mesh;
    mesh.positions = {{-1, 0, -1}, {-1, 0, 1}, {1, 0, 1}, {1, 0, -1}};
    mesh.triangles.push_back({{{{0, {0, 1, 0}, {}}, {1, {0, 1, 0}, {}}, {2, {0, 1, 0}, {}}}}});
    mesh.triangles.push_back({{{{0, {0, 1, 0}, {}}, {2, {0, 1, 0}, {}}, {3, {0, 1, 0}, {}}}}});
    scene.meshes.push_back({"plane", {}, std::make_shared<const MeshData>(std::move(mesh))});
    scene.materials.push_back({"floor", "floor"});
    scene.materials[0].kind = kind;
    scene.materials[0].base_color = glm::vec3(color);
    Transform transform;
    transform.scale = {4, 1, 4};
    scene.objects.push_back({"floor", "floor", "plane", "floor", transform, true});
    return scene;
}

SceneDocument direct_scene(bool glossy) {
    auto scene =
        floor_scene(glossy ? MaterialKind::conductor : MaterialKind::diffuse, glossy ? 0.8F : 0.7F);
    scene.environment = glm::vec3(0);
    scene.materials[0].roughness = 0.15F;
    scene.materials.push_back({"lamp", "lamp"});
    scene.materials[1].emission = glm::vec3(glossy ? 1 : 16);
    Transform transform;
    transform.translation = {glossy ? 0 : 0.75F, 2, 0};
    transform.scale = {glossy ? 2.5F : 0.125F, 1, glossy ? 2.5F : 0.125F};
    scene.objects.push_back({"lamp", "lamp", "plane", "lamp", transform, true});
    return scene;
}

struct Moments {
    std::size_t count = 0;
    double mean = 0, m2 = 0;
    void add(double value) {
        ++count;
        const double difference = value - mean;
        mean += difference / static_cast<double>(count);
        m2 += difference * (value - mean);
    }
    double variance() const {
        return count > 1 ? m2 / static_cast<double>(count - 1) : 0;
    }
    double error() const {
        return std::sqrt(variance() / static_cast<double>(count));
    }
};

Moments estimate(const PathTracer& tracer, std::size_t count, std::uint64_t seed,
                 PathStatistics* stats = nullptr) {
    Moments result;
    for (std::size_t sample = 0; sample < count; ++sample) {
        auto random = Sampler::for_sample(seed, 0, sample);
        result.add(tracer.trace({{0, 1, 0}, {0, -1, 0}}, random, {0, 10000}, stats).x);
    }
    return result;
}

} // namespace

TEST_CASE("Finite shadow segments retain precision near their sampled light endpoint",
          "[path][precision]") {
    const auto scene = make_snapshot(emitter_scene());
    const SceneBvh bvh(scene);
    const LightSampler lights(scene);
    const glm::dvec3 origin{0.5, 0.25, 4};
    Sampler random(171);
    int false_occlusion = 0;
    for (int index = 0; index < 1000; ++index) {
        const auto u = random.next_2d();
        const auto light = lights.sample(origin, 0.5, u);
        REQUIRE(light);
        const Ray ray{origin, light->direction};
        false_occlusion += bvh.occluded(ray, {0, light->distance * (1 - 1e-9)});
    }
    INFO("False endpoint occlusions: " << false_occlusion);
    REQUIRE(false_occlusion == 0);
}

TEST_CASE("Power heuristic handles limiting probabilities without squaring overflow", "[path]") {
    REQUIRE(power_heuristic(1, 1) == 0.5);
    REQUIRE(power_heuristic(1e300, 1e300) == 0.5);
    REQUIRE(power_heuristic(3, 4) == Approx(9.0 / 25));
    REQUIRE(power_heuristic(0, 1) == 0);
    REQUIRE(power_heuristic(1, 0) == 1);
    REQUIRE(power_heuristic(0, 0) == 0);
    REQUIRE_THROWS(power_heuristic(-1, 1));
    REQUIRE(parse_sampling_mode("mis") == SamplingMode::mis);
    REQUIRE_THROWS(parse_sampling_mode("unknown"));
}

TEST_CASE("Diffuse constant environment agrees across estimators without self-intersection",
          "[path]") {
    const auto snapshot = make_snapshot(floor_scene());
    for (const auto mode : {SamplingMode::bsdf, SamplingMode::light, SamplingMode::mis}) {
        const PathTracer tracer(snapshot, {mode, 1, false, 3});
        const auto values = estimate(tracer, 8192, 81);
        const double expected = static_cast<double>(snapshot->instances[0].material.base_color.x);
        INFO(sampling_mode_name(mode) << " mean " << values.mean << " SE " << values.error());
        REQUIRE(std::abs(values.mean - expected) < 5 * values.error() + 1e-8);
    }
    const SceneBvh bvh(snapshot);
    const auto hit = bvh.intersect({{0, 1, 0}, {0, -1, 0}});
    REQUIRE(hit);
    REQUIRE_FALSE(bvh.occluded(spawn_ray(*hit, {0, 1, 0}), {0, 100}));
    for (const float scale : {0.001F, 1.0F, 1000.0F}) {
        auto scene = floor_scene();
        scene.objects[0].transform.scale *= scale;
        scene.objects[0].transform.translation = glm::vec3(scale * 7);
        const PathTracer tracer(make_snapshot(scene), {SamplingMode::bsdf, 1, false, 3});
        Sampler random(31);
        const auto result =
            tracer.trace({{scale * 7, scale * 8, scale * 7}, {0, -1, 0}}, random, {0, 10000});
        REQUIRE(result.x == Approx(0.6F).margin(1e-8));
    }
}

TEST_CASE("Directional lighting remains unbiased in the BSDF baseline", "[path]") {
    auto scene = floor_scene();
    scene.environment = glm::vec3(0);
    const float irradiance = static_cast<float>(std::numbers::pi);
    scene.directional_lights.push_back({"sun", {0, -1, 0}, {1, 1, 1}, irradiance});
    const double expected =
        static_cast<double>(scene.materials[0].base_color.x) * irradiance / std::numbers::pi;
    for (const auto mode : {SamplingMode::bsdf, SamplingMode::light, SamplingMode::mis}) {
        const PathTracer tracer(make_snapshot(scene), {mode, 1, false, 3});
        const auto values = estimate(tracer, 32, 3);
        REQUIRE(values.mean == Approx(expected).margin(1e-10));
    }
}

TEST_CASE("Delta mirror and closed glass paths preserve environment transport", "[path]") {
    auto mirror = floor_scene(MaterialKind::mirror, 0.7F);
    for (const auto mode : {SamplingMode::bsdf, SamplingMode::light, SamplingMode::mis}) {
        const PathTracer tracer(make_snapshot(mirror), {mode, 2, false, 3});
        REQUIRE(estimate(tracer, 128, 9).mean == Approx(0.7F).margin(1e-10));
    }
    auto glass = floor_scene(MaterialKind::dielectric, 1);
    auto bottom = glass.objects[0];
    bottom.id = "bottom";
    bottom.transform.translation.y = -1;
    bottom.transform.rotation_degrees.x = 180;
    glass.objects.push_back(bottom);
    for (const auto mode : {SamplingMode::bsdf, SamplingMode::light, SamplingMode::mis}) {
        const PathTracer tracer(make_snapshot(glass), {mode, 12, false, 3});
        REQUIRE(estimate(tracer, 1024, 125).mean == Approx(1).margin(1e-8));
    }
}

TEST_CASE("Depth zero, black scenes and emission are independent of light sampling", "[path]") {
    auto scene = floor_scene();
    scene.environment = glm::vec3(0);
    scene.materials[0].emission = {8, 2, 0.5F};
    PathStatistics stats;
    const PathTracer tracer(make_snapshot(scene), {SamplingMode::mis, 0, false, 3});
    Sampler random(9);
    REQUIRE(tracer.trace({{0, 1, 0}, {0, -1, 0}}, random, {0, 100}, &stats) ==
            glm::dvec3(8, 2, 0.5));
    REQUIRE(stats.scattering_events == 0);
    REQUIRE(stats.shadow_rays == 0);
    scene.materials[0].emission = glm::vec3(0);
    REQUIRE(estimate(PathTracer(make_snapshot(scene)), 32, 10).mean == 0);
}

TEST_CASE("Russian roulette changes work without changing the multi-bounce mean", "[path]") {
    auto scene = floor_scene(MaterialKind::diffuse, 0.5F);
    scene.environment = glm::vec3(0);
    scene.materials[0].emission = glm::vec3(1);
    scene.objects[0].transform.scale = {1000, 1, 1000};
    auto ceiling = scene.objects[0];
    ceiling.id = "ceiling";
    ceiling.transform.translation.y = 2;
    scene.objects.push_back(ceiling);
    const auto snapshot = make_snapshot(scene);
    PathStatistics no_rr_stats, rr_stats;
    const auto no_rr =
        estimate(PathTracer(snapshot, {SamplingMode::bsdf, 8, false, 1}), 4096, 13, &no_rr_stats);
    const auto rr =
        estimate(PathTracer(snapshot, {SamplingMode::bsdf, 8, true, 1}), 16384, 14, &rr_stats);
    REQUIRE(std::abs(rr.mean - no_rr.mean) < 5 * std::hypot(rr.error(), no_rr.error()) + 0.002);
    REQUIRE(no_rr.mean == Approx(2 - 1.0 / 256).margin(0.003));
    REQUIRE(rr_stats.roulette_terminations > 0);
    REQUIRE(static_cast<double>(rr_stats.scattering_events) / static_cast<double>(rr_stats.paths) <
            static_cast<double>(no_rr_stats.scattering_events) /
                static_cast<double>(no_rr_stats.paths));
}

TEST_CASE("Thin-lens rays converge on the specified focus plane", "[camera][path]") {
    Camera camera;
    camera.position = {0, 0, 5};
    camera.target = {0, 0, 0};
    const glm::dvec2 film{0.7, 0.4};
    const auto pinhole = camera.generate_ray(film, 1.5);
    REQUIRE(camera.generate_lens_ray(film, 1.5, {0.1, 0.9}).direction == pinhole.direction);
    camera.aperture_radius = 0.2F;
    camera.focus_distance = 3;
    const auto focus = pinhole.origin + pinhole.direction * (3 / -pinhole.direction.z);
    for (const auto u : {glm::dvec2(0.1, 0.2), glm::dvec2(0.5), glm::dvec2(0.8, 0.9)}) {
        const auto ray = camera.generate_lens_ray(film, 1.5, u);
        REQUIRE(glm::length(ray.direction) == Approx(1).margin(1e-12));
        REQUIRE(glm::length(ray.origin - glm::dvec3(camera.position)) <= camera.aperture_radius);
        const auto point = ray.origin + ray.direction * ((2 - ray.origin.z) / ray.direction.z);
        REQUIRE(glm::length(point - focus) < 1e-12);
    }
    camera.focus_distance = 0;
    REQUIRE_THROWS(camera.validate());
}

TEST_CASE("Finite shadows remain consistent after large scene translation", "[path][precision]") {
    auto scene = direct_scene(false);
    const PathTracer original(make_snapshot(scene), {SamplingMode::light, 1, false, 3});
    for (auto& object : scene.objects) {
        object.transform.translation = object.transform.translation * 128.0F + glm::vec3(1e8F);
        object.transform.scale *= 128.0F;
    }
    const PathTracer moved(make_snapshot(scene), {SamplingMode::light, 1, false, 3});
    for (std::uint64_t sample = 0; sample < 512; ++sample) {
        auto a = Sampler::for_sample(5, 0, sample), b = a;
        const auto expected = original.trace({{0, 1, 0}, {0, -1, 0}}, a);
        const auto actual = moved.trace({{1e8, 1e8 + 128, 1e8}, {0, -1, 0}}, b);
        REQUIRE(glm::length(actual - expected) < 1e-7);
    }
}

TEST_CASE("Shadow visibility distinguishes intervening geometry from geometry beyond the light",
          "[path]") {
    auto scene = direct_scene(false);
    const PathTracer clear(make_snapshot(scene), {SamplingMode::light, 1, false, 3});
    scene.materials.push_back({"black", "black"});
    scene.materials.back().base_color = glm::vec3(0);
    auto blocker = scene.objects[0];
    blocker.id = "blocker";
    blocker.material_id = "black";
    blocker.transform.translation.y = 1;
    scene.objects.push_back(blocker);
    const PathTracer blocked(make_snapshot(scene), {SamplingMode::light, 1, false, 3});
    scene.objects.back().transform.translation.y = 3;
    const PathTracer behind(make_snapshot(scene), {SamplingMode::light, 1, false, 3});
    for (std::uint64_t sample = 0; sample < 256; ++sample) {
        auto a = Sampler::for_sample(79, 0, sample), b = a, c = a;
        const Ray ray{{0, 0.5, 0}, {0, -1, 0}};
        const auto value = clear.trace(ray, a);
        REQUIRE(value.x > 0);
        REQUIRE(blocked.trace(ray, b) == glm::dvec3(0));
        REQUIRE(glm::length(behind.trace(ray, c) - value) < 1e-12);
    }
}

TEST_CASE("MIS preserves means and reduces variance in complementary lighting cases",
          "[path][mis]") {
    constexpr std::size_t seeds = 12, samples = 2048;
    for (const bool glossy : {false, true}) {
        const auto snapshot = make_snapshot(direct_scene(glossy));
        const PathTracer bsdf(snapshot, {SamplingMode::bsdf, 1, false, 3});
        const PathTracer light(snapshot, {SamplingMode::light, 1, false, 3});
        const PathTracer mis(snapshot, {SamplingMode::mis, 1, false, 3});
        double reference = 0, reference_error = 0;
        if (glossy) {
            const auto high = estimate(bsdf, 65536, 991);
            reference = high.mean;
            reference_error = high.error();
        } else {
            // 独立面积积分：Le*rho/pi * ∫ cos(theta_r) cos(theta_l)/r^2 dA。
            constexpr int steps = 80;
            const double width = 0.25;
            for (int row = 0; row < steps; ++row) {
                for (int column = 0; column < steps; ++column) {
                    const double x = 0.625 + width * (column + 0.5) / steps;
                    const double z = -0.125 + width * (row + 0.5) / steps;
                    const double r2 = x * x + 4 + z * z;
                    reference += 4 / (r2 * r2) * (width * width) / (steps * steps);
                }
            }
            reference *= 16 * static_cast<double>(snapshot->instances[0].material.base_color.x) /
                         std::numbers::pi;
        }
        std::array<Moments, 3> batches;
        for (std::size_t seed = 0; seed < seeds; ++seed) {
            batches[0].add(estimate(bsdf, samples, seed + 17).mean);
            batches[1].add(estimate(light, samples, seed + 17).mean);
            batches[2].add(estimate(mis, samples, seed + 17).mean);
        }
        std::cout << (glossy ? "glossy-large-light" : "diffuse-small-light") << " reference "
                  << reference << " reference SE " << reference_error << " seeds " << seeds
                  << " samples/seed " << samples << '\n';
        for (std::size_t mode = 0; mode < batches.size(); ++mode) {
            const auto& values = batches[mode];
            std::cout << "  " << sampling_mode_name(static_cast<SamplingMode>(mode)) << " mean "
                      << values.mean << " variance " << values.variance() << " SE "
                      << values.error() << '\n';
            INFO("glossy=" << glossy << " mode=" << mode << " reference=" << reference
                           << " mean=" << values.mean << " SE=" << values.error());
            REQUIRE(std::abs(values.mean - reference) <
                    5 * std::hypot(values.error(), reference_error) + 1e-5);
        }
        REQUIRE(batches[2].variance() < batches[glossy ? 1 : 0].variance());
    }
}

TEST_CASE("Thin-lens parameters survive scene serialization", "[camera][path][io]") {
    SceneDocument scene;
    scene.camera.aperture_radius = 0.03F;
    scene.camera.focus_distance = 2.5F;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto file = std::filesystem::temp_directory_path() /
                      ("minirender-lens-" + std::to_string(stamp) + ".json");
    save_scene(scene, file);
    const auto loaded = load_scene(file);
    std::filesystem::remove(file);
    REQUIRE(loaded.camera.aperture_radius == scene.camera.aperture_radius);
    REQUIRE(loaded.camera.focus_distance == scene.camera.focus_distance);
    const auto legacy =
        load_scene(std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes/studio.json");
    REQUIRE(legacy.camera.aperture_radius == 0);
}

TEST_CASE("Pixel rendering reuses deterministic samples and keeps HDR separate from display",
          "[path]") {
    auto scene = floor_scene();
    const PathTracer tracer(make_snapshot(scene), {SamplingMode::mis, 2, false, 3});
    const RenderSettings settings{4, 3, 8, 29};
    const auto image = tracer.render(settings);
    glm::dvec3 sum(0);
    for (std::uint64_t sample = 0; sample < settings.samples_per_pixel; ++sample) {
        sum += tracer.sample_pixel(1, 1, sample, settings);
    }
    REQUIRE(image.at(1, 1) == glm::vec3(sum / 8.0));
    REQUIRE(tracer.sample_pixel(1, 1, 7, settings) == tracer.sample_pixel(1, 1, 7, settings));
    Image hdr(1, 1);
    hdr.at(0, 0) = {8, 2, 0.5F};
    const auto display = display_image(hdr);
    REQUIRE(display.at(0, 0).x == Approx(8.0 / 9).margin(1e-7));
    REQUIRE(display_image(hdr, 2).at(0, 0).y > display.at(0, 0).y);
    REQUIRE(hdr.at(0, 0) == glm::vec3(8, 2, 0.5F));
}
