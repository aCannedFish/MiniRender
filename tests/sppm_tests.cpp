#include "render/sppm_grid.hpp"
#include "render/sppm_surface.hpp"
#include <minirender/assets/scene_io.hpp>
#include <minirender/render/render_session.hpp>
#include <minirender/render/sppm.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <set>
#include <thread>

using namespace minirender;
using Catch::Approx;
namespace {
constexpr double pi = std::numbers::pi;
SceneDocument plane(bool emitter = false, bool double_sided = false) {
    SceneDocument scene;
    scene.camera.position = {0, 1, 0};
    scene.camera.target = {0, 0, 0};
    scene.camera.up = {0, 0, -1};
    scene.environment = glm::vec3(emitter ? 0 : 1);
    auto mesh = std::make_shared<MeshData>();
    mesh->positions = {{-1, 0, -1}, {-1, 0, 1}, {1, 0, 1}, {1, 0, -1}};
    mesh->triangles = {{{{{0, {0, 1, 0}, {}}, {1, {0, 1, 0}, {}}, {2, {0, 1, 0}, {}}}}},
                       {{{{0, {0, 1, 0}, {}}, {2, {0, 1, 0}, {}}, {3, {0, 1, 0}, {}}}}}};
    scene.meshes.push_back({"plane", {}, mesh});
    Material material;
    material.id = "surface";
    material.base_color = glm::vec3(.6F);
    material.emission = glm::vec3(emitter ? 2 : 0);
    material.double_sided = double_sided;
    scene.materials.push_back(material);
    scene.objects.push_back({"plane", "plane", "plane", "surface", {}});
    return scene;
}
IntegratorSettings options(std::uint32_t depth = 6) {
    IntegratorSettings result;
    result.kind = IntegratorKind::sppm;
    result.maximum_depth = depth;
    result.russian_roulette = false;
    result.sppm.photons_per_iteration = 2000;
    result.sppm.initial_radius = .3;
    return result;
}
PhotonEmission emission(const LightSampler& light, Sampler& random) {
    const auto select = random.next_1d();
    const auto position = random.next_2d();
    const auto direction = random.next_2d();
    const auto sample = light.sample_emission(select, position, direction);
    REQUIRE(sample);
    return *sample;
}
double difference(const Image& a, const Image& b) {
    double maximum = 0;
    for (std::size_t y = 0; y < a.height(); ++y) {
        for (std::size_t x = 0; x < a.width(); ++x) {
            maximum = std::max(maximum, double(glm::length(a.at(x, y) - b.at(x, y))));
        }
    }
    return maximum;
}
void same(std::span<const SppmPixel> a, std::span<const SppmPixel> b) {
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        REQUIRE(a[i].radius == b[i].radius);
        REQUIRE(a[i].tau == b[i].tau);
        REQUIRE(a[i].effective_photons == b[i].effective_photons);
        REQUIRE(a[i].direct_sum == b[i].direct_sum);
    }
}
} // namespace

TEST_CASE("Area photon emission conserves power and samples the selected hemispheres",
          "[sppm][emission]") {
    for (const bool two_sided : {false, true}) {
        const auto snapshot = make_snapshot(plane(true, two_sided));
        const LightSampler light(snapshot);
        const SceneBvh bvh(snapshot);
        Sampler random(101);
        glm::dvec3 center(0), flux(0);
        double cosine = 0;
        int back = 0, self_hits = 0;
        constexpr int samples = 30000;
        for (int i = 0; i < samples; ++i) {
            const auto value = emission(light, random);
            center += value.ray.origin;
            flux += value.flux;
            cosine += std::abs(value.ray.direction.y);
            back += value.ray.direction.y < 0;
            self_hits += bvh.occluded(value.ray, {0, 100});
            REQUIRE(value.position_pdf == .5);
            REQUIRE(value.direction_pdf ==
                    Approx(std::abs(value.ray.direction.y) / (pi * (two_sided ? 2 : 1)))
                        .epsilon(1e-12));
        }
        REQUIRE(glm::length(center / double(samples)) < .02);
        REQUIRE(glm::length(flux / double(samples) - glm::dvec3((two_sided ? 16 : 8) * pi)) < 1e-8);
        REQUIRE(cosine / samples == Approx(2.0 / 3).margin(.008));
        REQUIRE((two_sided ? std::abs(back - samples / 2) < 500 : back == 0));
        REQUIRE(self_hits == 0);
    }
}

TEST_CASE("Infinite photon emission accounts for projected area and missed photons",
          "[sppm][emission]") {
    auto scene = plane();
    scene.environment = glm::vec3(0);
    scene.directional_lights.push_back({"sun", {0, -1, 0}, {1, 1, 1}, 3});
    const auto snapshot = make_snapshot(scene);
    const SceneBvh bvh(snapshot);
    const LightSampler light(snapshot);
    Sampler random(917);
    glm::dvec3 irradiance(0);
    constexpr int samples = 50000;
    int hits = 0;
    for (int i = 0; i < samples; ++i) {
        const auto value = emission(light, random);
        REQUIRE(value.ray.direction == glm::dvec3(0, -1, 0));
        REQUIRE(value.position_pdf == Approx(1 / (2 * pi)).epsilon(1e-12));
        REQUIRE(value.flux.x == Approx(6 * pi).epsilon(1e-12));
        if (bvh.intersect(value.ray, {0, 100})) {
            irradiance += value.flux / 4.0;
            ++hits;
        }
    }
    REQUIRE(double(hits) / samples == Approx(2 / pi).margin(.008));
    REQUIRE(irradiance.x / samples == Approx(3).margin(.04));
    scene.directional_lights.clear();
    scene.environment = glm::vec3(1);
    const LightSampler sky(make_snapshot(scene));
    glm::dvec3 direction_sum(0);
    for (int i = 0; i < samples; ++i) {
        const auto value = emission(sky, random);
        direction_sum += value.ray.direction;
        REQUIRE(value.flux.x == Approx(8 * pi * pi).epsilon(1e-12));
        REQUIRE(value.direction_pdf == Approx(1 / (4 * pi)).epsilon(1e-12));
    }
    REQUIRE(glm::length(direction_sum / double(samples)) < .015);
}

TEST_CASE("Emission light mixture retains RGB power through its selection PMF",
          "[sppm][emission]") {
    auto scene = plane(true);
    scene.materials[0].emission = {2, 0, 0};
    scene.directional_lights.push_back({"sun", {0, -1, 0}, {0, 1, 0}, 3});
    scene.environment = {0, 0, 1};
    const LightSampler lights(make_snapshot(scene));
    Sampler random(781);
    glm::dvec3 mean(0);
    constexpr int count = 100000;
    for (int i = 0; i < count; ++i) {
        mean += emission(lights, random).flux / double(count);
    }
    REQUIRE(mean.x == Approx(8 * pi).margin(.5));
    REQUIRE(mean.y == Approx(6 * pi).margin(.3));
    REQUIRE(mean.z == Approx(8 * pi * pi).margin(1));
}

TEST_CASE("SPPM grid agrees with brute force and never repeats a visible point", "[sppm][grid]") {
    Sampler random(91);
    std::vector<detail::SppmGridPoint> points;
    for (std::size_t i = 0; i < 200; ++i) {
        const auto a = random.next_2d();
        const auto b = random.next_2d();
        points.push_back(
            {glm::dvec3(1e6, 2e6, -1e6) + glm::dvec3(a.x, a.y, b.x), .01 + .2 * b.y, i});
    }
    const detail::SppmGrid grid(points);
    for (int i = 0; i < 1000; ++i) {
        const auto a = random.next_2d();
        const auto b = random.next_2d();
        const auto position = i < 200 ? points[std::size_t(i)].position
                                      : glm::dvec3(1e6, 2e6, -1e6) + glm::dvec3(a.x, a.y, b.x);
        std::set<std::size_t> expected, actual;
        std::size_t callbacks = 0;
        for (const auto& point : points) {
            if (glm::length(position - point.position) <= point.radius) {
                expected.insert(point.index);
            }
        }
        grid.query(position, [&](auto index) {
            actual.insert(index);
            ++callbacks;
        });
        REQUIRE(actual == expected);
        REQUIRE(callbacks == actual.size());
    }
}

TEST_CASE("SPPM progressive update matches independent count area and flux equations", "[sppm]") {
    const SppmPixel before{.4, 10, {1, 2, 3}, {.1, .2, .3}};
    const auto after = update_sppm_pixel(before, 20, {4, 5, 6}, {.2, .3, .4}, .7);
    REQUIRE(after.effective_photons == 24);
    REQUIRE(after.radius == Approx(.4 * std::sqrt(.8)).epsilon(1e-14));
    REQUIRE(glm::length(after.tau - glm::dvec3(4, 5.6, 7.2)) < 1e-12);
    REQUIRE(glm::length(after.direct_sum - glm::dvec3(.3, .5, .7)) < 1e-12);
    const auto empty = update_sppm_pixel(before, 0, {}, glm::dvec3(1), .7);
    REQUIRE(empty.radius == before.radius);
    REQUIRE(empty.tau == before.tau);
    REQUIRE(empty.effective_photons == before.effective_photons);
    REQUIRE_THROWS_AS(update_sppm_pixel(before, 1, {}, {}, 1), std::invalid_argument);
}

TEST_CASE("SPPM direct lighting is counted once and matches the existing light estimator",
          "[sppm]") {
    const auto snapshot = make_snapshot(plane());
    auto config = options(1);
    const RenderSettings film{8, 8, 16, 101};
    SppmIntegrator renderer(snapshot, film, config);
    for (std::uint32_t i = 0; i < film.samples_per_pixel; ++i) {
        renderer.advance();
    }
    IntegratorSettings reference{SamplingMode::light, 1, false, 3};
    const auto expected = PathTracer(snapshot, reference).render(film);
    REQUIRE(difference(renderer.image(), expected) < 1e-6);
    REQUIRE(renderer.statistics().emitted_photons ==
            std::uint64_t(film.samples_per_pixel) * config.sppm.photons_per_iteration);
    for (const auto& pixel : renderer.pixels()) {
        REQUIRE(pixel.tau == glm::dvec3(0));
        REQUIRE(pixel.effective_photons == 0);
        REQUIRE(pixel.radius == config.sppm.initial_radius);
    }
}

TEST_CASE("SPPM cancellation and callback exceptions keep the complete previous iteration",
          "[sppm]") {
    const auto scene = make_snapshot(
        load_scene(std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes/cornell-box.json"));
    const RenderSettings film{8, 8, 3, 113};
    const auto config = options();
    SppmIntegrator renderer(scene, film, config), reference(scene, film, config);
    renderer.advance();
    reference.advance();
    const auto image = renderer.image();
    const auto previous =
        std::vector<SppmPixel>(renderer.pixels().begin(), renderer.pixels().end());
    CancellationSource stop;
    REQUIRE_THROWS_AS(renderer.advance(stop.token(),
                                       [&](auto progress) {
                                           if (progress.phase == SppmPhase::photons &&
                                               progress.completed > 0) {
                                               stop.request();
                                           }
                                       }),
                      OperationCancelled);
    same(renderer.pixels(), previous);
    REQUIRE(renderer.statistics().iterations == 1);
    REQUIRE(difference(renderer.image(), image) == 0);
    REQUIRE_THROWS_AS(
        renderer.advance({}, [](auto) { throw std::runtime_error("progress error"); }),
        std::runtime_error);
    same(renderer.pixels(), previous);
    renderer.advance();
    reference.advance();
    same(renderer.pixels(), reference.pixels());
}

TEST_CASE("SPPM parallel photon streams preserve counts radii and image", "[sppm]") {
    const auto scene = make_snapshot(
        load_scene(std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes/cornell-box.json"));
    const RenderSettings film{8, 8, 8, 211};
    const auto config = options();
    SppmIntegrator serial(scene, film, config, 1), parallel(scene, film, config, 3);
    for (int i = 0; i < 8; ++i) {
        serial.advance();
        parallel.advance();
    }
    REQUIRE(serial.statistics().gathered_contributions > 0);
    REQUIRE(serial.statistics().gathered_contributions ==
            parallel.statistics().gathered_contributions);
    REQUIRE(serial.statistics().mean_radius < config.sppm.initial_radius);
    for (std::size_t i = 0; i < serial.pixels().size(); ++i) {
        REQUIRE(serial.pixels()[i].radius == parallel.pixels()[i].radius);
        REQUIRE(serial.pixels()[i].effective_photons == parallel.pixels()[i].effective_photons);
    }
    REQUIRE(difference(serial.image(), parallel.image()) < 1e-5);
}

TEST_CASE("Cloth importance sampling uses the transposed radiance kernel", "[sppm][cloth-bsdf]") {
    Material material;
    material.kind = MaterialKind::cloth;
    material.cloth = make_cloth_preset("silk").appearance;
    const Bsdf bsdf(material);
    const auto wo = glm::normalize(glm::dvec3(.3, .2, 1));
    Sampler random(51);
    int checked = 0;
    for (int i = 0; i < 2000; ++i) {
        const double component = random.next_1d();
        const auto u = random.next_2d();
        const auto value = bsdf.sample(wo, component, u, TransportMode::importance);
        if (!value) {
            continue;
        }
        const auto expected =
            bsdf.evaluate(value->direction, wo) * value->direction.z / value->probability;
        REQUIRE(glm::length(value->weight - expected) <
                1e-9 * std::max(1.0, glm::length(expected)));
        ++checked;
    }
    REQUIRE(checked > 1000);
}

TEST_CASE("SPPM rejects unsupported camera media and over-budget work before allocating",
          "[sppm]") {
    auto scene = plane();
    auto config = options();
    REQUIRE_THROWS_AS(SppmIntegrator(make_snapshot(scene), {1024, 1024, 1, 1}, config, 32),
                      std::length_error);
    scene.camera.aperture_radius = .01F;
    REQUIRE_THROWS_AS(SppmIntegrator(make_snapshot(scene), {1, 1, 1, 1}, config),
                      std::invalid_argument);
    scene.camera.aperture_radius = 0;
    MediumRegion region;
    region.id = "fog";
    region.medium.scattering = glm::dvec3(.2);
    scene.media.push_back(region);
    REQUIRE_THROWS_AS(SppmIntegrator(make_snapshot(scene), {1, 1, 1, 1}, config),
                      std::invalid_argument);
}

TEST_CASE("Glass and mirror scenes record nonzero specular diffuse specular merges",
          "[sppm][caustic]") {
    auto scene =
        load_scene(std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes/cornell-materials.json");
    for (const bool mirror : {false, true}) {
        if (mirror) {
            for (auto& material : scene.materials) {
                if (material.id == "mat_back") {
                    material.kind = MaterialKind::mirror;
                }
            }
        }
        auto config = options(8);
        config.sppm.photons_per_iteration = 5000;
        SppmIntegrator renderer(make_snapshot(scene), {8, 8, 16, 101}, config, 2);
        for (int i = 0; i < 16; ++i) {
            renderer.advance();
        }
        REQUIRE(renderer.statistics().specular_light_contributions > 0);
        REQUIRE(renderer.statistics().sds_contributions > 0);
    }
}

TEST_CASE("SPPM shading normal conversion matches the radiance operator transpose", "[sppm]") {
    const auto scene = make_snapshot(plane());
    SurfaceHit hit;
    hit.geometric_normal = {0, 1, 0};
    hit.shading_normal = glm::normalize(glm::vec3(0, .8F, .6F));
    const auto wo = glm::normalize(glm::dvec3(.1, 1, .2));
    const detail::SppmSurface surface(*scene, hit, wo);
    const auto ns = glm::normalize(glm::dvec3(hit.shading_normal));
    const glm::dvec3 ng{0, 1, 0};
    Sampler random(17);
    int checked = 0;
    for (int i = 0; i < 1000; ++i) {
        const double component = random.next_1d();
        const auto u = random.next_2d();
        const auto sample = surface.sample(component, u, TransportMode::importance);
        if (!sample) {
            continue;
        }
        const auto wi = sample->direction;
        const double f = double(.6F) / pi;
        const auto expected = glm::dvec3(f * glm::dot(ns, wo) * glm::dot(ng, wi) /
                                         glm::dot(ng, wo) / sample->probability);
        REQUIRE(glm::length(sample->weight - expected) < 1e-9);
        REQUIRE(glm::length(surface.gather(wi) * glm::dot(ng, wi) - surface.response(wi)) < 1e-12);
        ++checked;
    }
    REQUIRE(checked > 500);
}

TEST_CASE("SPPM background task shares previews pause rollback resume and cancellation",
          "[sppm][session]") {
    const auto snapshot = make_snapshot(
        load_scene(std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes/cornell-box.json"));
    auto config = options();
    const RenderSettings film{8, 8, 4, 101};
    SppmIntegrator reference(snapshot, film, config, 2);
    for (int i = 0; i < 4; ++i) {
        reference.advance();
    }
    RenderSession session;
    session.start(snapshot, film, config, {2, 8, 2});
    REQUIRE(session.wait_for(std::chrono::seconds(5)));
    session.wait();
    REQUIRE(session.progress().state == RenderState::completed);
    REQUIRE(session.progress().sppm.iterations == 4);
    REQUIRE(session.progress().recorded_samples == 4 * 8 * 8);
    REQUIRE(difference(*session.result(), reference.image()) < 1e-6);
    config.sppm.photons_per_iteration = 50000;
    session.start(snapshot, {16, 16, 1000000, 11}, config, {2, 8, 2});
    const auto wait = [&](const std::function<bool()>& condition) {
        const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!condition() && std::chrono::steady_clock::now() < limit) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        REQUIRE(condition());
    };
    wait([&] { return session.progress().sppm.iterations > 0; });
    REQUIRE(session.pause());
    wait([&] { return session.progress().state == RenderState::paused; });
    const auto paused = session.progress();
    REQUIRE(paused.recorded_samples == paused.sppm.iterations * 16 * 16);
    const auto image = session.preview(0);
    REQUIRE(image.image);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(session.progress().recorded_samples == paused.recorded_samples);
    REQUIRE(session.resume());
    wait([&] { return session.progress().recorded_samples > paused.recorded_samples; });
    session.cancel();
    REQUIRE(session.wait_for(std::chrono::seconds(2)));
    session.wait();
    REQUIRE(session.progress().state == RenderState::cancelled);
    REQUIRE(session.progress().recorded_samples == session.progress().sppm.iterations * 16 * 16);
    REQUIRE(session.preview(0).image);
}
