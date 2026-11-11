#include <minirender/assets/scene_io.hpp>
#include <minirender/core/sampling.hpp>
#include <minirender/geometry/uv_tangents.hpp>
#include <minirender/render/cloth_bsdf.hpp>
#include <minirender/render/path_tracer.hpp>
#include <minirender/simulation/cloth.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <fstream>
#include <numbers>

using namespace minirender;
using Catch::Approx;
namespace {
constexpr double pi = std::numbers::pi;

glm::dvec3 rotate(glm::dvec3 v, double angle) {
    return {std::cos(angle) * v.x - std::sin(angle) * v.y,
            std::sin(angle) * v.x + std::cos(angle) * v.y, v.z};
}

// 均匀 z/phi 中点积分，测度为 dphi*dz，与纱线提议分布独立。
glm::dvec3 integrate(const ClothBsdf& bsdf, glm::dvec3 wo, int z_steps = 128, int phi_steps = 256) {
    glm::dvec3 result(0);
    for (int z = 0; z < z_steps; ++z) {
        const double cosine = (z + .5) / z_steps;
        const double sine = std::sqrt(1 - cosine * cosine);
        for (int phi = 0; phi < phi_steps; ++phi) {
            const double angle = 2 * pi * (phi + .5) / phi_steps;
            result += bsdf.response(wo, {sine * std::cos(angle), sine * std::sin(angle), cosine});
        }
    }
    return result * (2 * pi / (z_steps * phi_steps));
}

SceneDocument cloth_scene() {
    auto grid = ClothGrid();
    grid.columns = grid.rows = 2;
    grid.origin = {-1, -1, 0};
    grid.span_u = {2, 0, 0};
    grid.span_v = {0, 2, 0};
    const auto frozen = ClothSimulation(grid).freeze();
    const auto preset = make_cloth_preset("shot");
    Material material;
    material.id = "cloth";
    material.kind = MaterialKind::cloth;
    material.base_color = glm::vec3(1);
    material.ior = static_cast<float>(preset.ior);
    material.cloth = preset.appearance;
    SceneDocument scene;
    scene.environment = glm::vec3(0);
    scene.camera.position = {0, 0, 3};
    scene.camera.target = {0, 0, 0};
    scene.materials.push_back(material);
    scene.meshes.push_back({"quad", {}, frozen.mesh});
    scene.objects.push_back({"cloth", "Cloth", "quad", "cloth", frozen.transform});
    scene.directional_lights.push_back({"sun", {-.4F, -.15F, -1}, {1, 1, 1}, 2});
    return scene;
}

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        path = std::filesystem::temp_directory_path() /
               ("minirender-cloth-material-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    std::filesystem::path path;
};
} // namespace

TEST_CASE("Cloth appearance compiles length weighted tangent curves and presets", "[cloth-bsdf]") {
    ClothAppearanceSettings settings;
    settings.samples_per_thread = 4;
    settings.threads[0].curve = {{-20, -20, 40, 40}, {1, 0, 3}};
    const ClothAppearance appearance(settings);
    REQUIRE(appearance.samples().size() == 8);
    double mean = 0, weight = 0;
    for (const auto& sample : appearance.samples()) {
        REQUIRE(sample.cosine * sample.cosine + sample.sine * sample.sine == Approx(1));
        weight += sample.weight;
        if (sample.thread == 0) {
            mean += sample.weight * std::atan2(sample.sine, sample.cosine) * 180 / pi;
        }
    }
    REQUIRE(weight == Approx(.66));
    REQUIRE(mean / settings.coverage[0] == Approx(25));
    for (const auto* name : {"linen", "silk", "satin", "shot"}) {
        const auto preset = make_cloth_preset(name);
        preset.appearance->settings().validate();
        REQUIRE(preset.ior > 1);
        REQUIRE(preset.appearance->coverage() <= 1);
    }
    settings.threads[0].curve.lengths = {0, 0, 0};
    REQUIRE_THROWS_AS(ClothAppearance(settings), std::invalid_argument);
    REQUIRE_THROWS_AS(make_cloth_preset("unknown"), std::invalid_argument);
}

TEST_CASE("Cloth flat thread has a closed form normal response and untinted surface lobe",
          "[cloth-bsdf]") {
    ClothAppearanceSettings settings;
    settings.coverage = {1, 0};
    settings.threads[0] = {{.2, .1, .05}, 1, 10, 20, {{0, 0}, {1}}};
    const auto appearance = std::make_shared<const ClothAppearance>(settings);
    const ClothBsdf bsdf(appearance, 1.5);
    const double f0 = .04;
    const double surface = f0 / (std::sqrt(2 * pi) * (10 * pi / 180));
    const auto expected =
        glm::dvec3(surface) + .5 * (1 - f0) * (1 - f0) * settings.threads[0].albedo;
    REQUIRE(glm::length(bsdf.evaluate({0, 0, 1}, {0, 0, 1}) - expected) < 1e-13);
    const ClothBsdf black(appearance, 1.5, glm::dvec3(0));
    REQUIRE(glm::length(black.evaluate({0, 0, 1}, {0, 0, 1}) - glm::dvec3(surface)) < 1e-13);
    const auto wi = glm::normalize(glm::dvec3(.3, .2, 1));
    REQUIRE(glm::length(bsdf.response({0, 0, 1}, wi) - bsdf.evaluate({0, 0, 1}, wi) * wi.z) <
            1e-13);
    REQUIRE(bsdf.evaluate({0, 0, 1}, {0, 0, -1}) == glm::dvec3(0));
    REQUIRE(bsdf.pdf({0, 0, 1}, {0, 0, -1}) == 0);
    REQUIRE_FALSE(bsdf.sample({0, 0, -1}, .5, {.5, .5}));
}

TEST_CASE("Cloth response and density rotate with UV directions and preserve yarn ordering",
          "[cloth-bsdf]") {
    const auto preset = make_cloth_preset("shot");
    const auto wo = glm::normalize(glm::dvec3(.3, -.2, 1));
    const auto wi = glm::normalize(glm::dvec3(-.1, .5, 1));
    const ClothAxes axes{{1, 0, 0}, {.6, .8, 0}};
    const ClothBsdf first(preset.appearance, preset.ior, glm::dvec3(1), axes);
    const double angle = .73;
    const ClothBsdf rotated(preset.appearance, preset.ior, glm::dvec3(1),
                            {rotate(axes.u, angle), rotate(axes.v, angle)});
    REQUIRE(glm::length(first.evaluate(wo, wi) -
                        rotated.evaluate(rotate(wo, angle), rotate(wi, angle))) < 1e-12);
    REQUIRE(first.pdf(wo, wi) == Approx(rotated.pdf(rotate(wo, angle), rotate(wi, angle))));
    auto settings = preset.appearance->settings();
    std::swap(settings.threads[0], settings.threads[1]);
    std::swap(settings.coverage[0], settings.coverage[1]);
    const ClothBsdf swapped(std::make_shared<const ClothAppearance>(settings), preset.ior,
                            glm::dvec3(1), {axes.v, axes.u});
    REQUIRE(glm::length(first.evaluate(wo, wi) - swapped.evaluate(wo, wi)) < 1e-12);
    REQUIRE(first.pdf(wo, wi) == Approx(swapped.pdf(wo, wi)));
}

TEST_CASE("Cloth mixed sampler matches analytic solid angle moments including null events",
          "[cloth-bsdf]") {
    const auto preset = make_cloth_preset("silk");
    const ClothBsdf bsdf(preset.appearance, preset.ior);
    const auto wo = glm::normalize(glm::dvec3(.3, .15, 1));
    constexpr int count = 120000;
    std::array<double, 4> sum{}, sum2{};
    int alive = 0;
    Sampler sampler(8471);
    for (int i = 0; i < count; ++i) {
        const double component = sampler.next_1d();
        const auto direction = bsdf.sample(wo, component, sampler.next_2d());
        if (!direction) {
            continue;
        }
        ++alive;
        const double density = bsdf.pdf(wo, *direction);
        REQUIRE(density > 0);
        REQUIRE(std::isfinite(density));
        const std::array values{1 / density, direction->x / density, direction->y / density,
                                direction->z / density};
        for (std::size_t j = 0; j < values.size(); ++j) {
            sum[j] += values[j];
            sum2[j] += values[j] * values[j];
        }
    }
    const std::array targets{2 * pi, 0.0, 0.0, pi};
    for (std::size_t j = 0; j < targets.size(); ++j) {
        const double mean = sum[j] / count;
        const double error = std::sqrt(std::max(0.0, sum2[j] / count - mean * mean) / count);
        INFO("moment=" << j << " mean=" << mean << " stderr=" << error);
        REQUIRE(std::abs(mean - targets[j]) < 5 * error + .005);
    }
    REQUIRE(alive > count / 3);
    REQUIRE(alive < 9 * count / 10);
}

TEST_CASE("Cloth BSDF sampling agrees with independent hemispherical integration", "[cloth-bsdf]") {
    const auto preset = make_cloth_preset("linen");
    const ClothBsdf bsdf(preset.appearance, preset.ior);
    const auto wo = glm::normalize(glm::dvec3(.4, -.15, 1));
    const auto reference = integrate(bsdf, wo);
    const auto refined = integrate(bsdf, wo, 256, 512);
    REQUIRE(glm::length(reference - refined) < .001);
    for (std::uint64_t seed = 1; seed <= 3; ++seed) {
        Sampler sampler(seed);
        glm::dvec3 sum(0), sum2(0);
        constexpr int count = 50000;
        for (int i = 0; i < count; ++i) {
            glm::dvec3 contribution(0);
            const double component = sampler.next_1d();
            if (const auto direction = bsdf.sample(wo, component, sampler.next_2d())) {
                contribution = bsdf.response(wo, *direction) / bsdf.pdf(wo, *direction);
            }
            sum += contribution;
            sum2 += contribution * contribution;
        }
        const auto mean = sum / double(count);
        const auto error =
            glm::sqrt(glm::max(glm::dvec3(0), sum2 / double(count) - mean * mean) / double(count));
        for (int channel = 0; channel < 3; ++channel) {
            INFO("seed=" << seed << " mean=" << mean[channel] << " target=" << refined[channel]);
            REQUIRE(std::abs(mean[channel] - refined[channel]) < 5 * error[channel] + .002);
        }
    }
}

TEST_CASE("UV directions preserve shear mirroring and coordinate scale", "[cloth-bsdf][uv]") {
    const std::array<glm::vec3, 3> positions{{{0, 0, 0}, {2, 0, 0}, {.4F, 1, 0}}};
    std::array<glm::vec2, 3> uv{{{0, 0}, {1, 0}, {0, 1}}};
    const auto first = uv_tangents(positions, uv);
    REQUIRE(first.has_value());
    REQUIRE(glm::length(first->u - glm::dvec3(1, 0, 0)) < 1e-12);
    REQUIRE(glm::length(first->v - glm::normalize(glm::dvec3(.4F, 1, 0))) < 1e-12);
    REQUIRE(glm::dot(first->u, first->v) > .3);
    uv[1].x = -1;
    const auto mirrored = uv_tangents(positions, uv);
    REQUIRE(mirrored.has_value());
    REQUIRE(mirrored->u == -first->u);
    REQUIRE(mirrored->v == first->v);
    for (auto& coordinate : uv) {
        coordinate *= 1e-20F;
    }
    const auto tiny = uv_tangents(positions, uv);
    REQUIRE(tiny.has_value());
    REQUIRE(glm::length(tiny->u - mirrored->u) < 1e-12);
    REQUIRE(glm::length(tiny->v - mirrored->v) < 1e-12);
    uv.fill(glm::vec2(0));
    REQUIRE_FALSE(uv_tangents(positions, uv));
}

TEST_CASE("Cloth thread importance sampling reduces satin variance", "[cloth-bsdf][sampling]") {
    const auto preset = make_cloth_preset("satin");
    const ClothBsdf bsdf(preset.appearance, preset.ior);
    const auto wo = glm::normalize(glm::dvec3(.15, .4, 1));
    const auto estimate = [&](bool importance) {
        Sampler sampler(2026);
        double sum = 0, squared = 0;
        constexpr int count = 60000;
        for (int i = 0; i < count; ++i) {
            const double component = sampler.next_1d();
            const auto u = sampler.next_2d();
            const auto direction = importance ? bsdf.sample(wo, component, u)
                                              : std::optional(sample_cosine_hemisphere(u));
            double contribution = 0;
            if (direction) {
                const double density =
                    importance ? bsdf.pdf(wo, *direction) : cosine_hemisphere_pdf(*direction);
                contribution =
                    glm::dot(bsdf.response(wo, *direction), glm::dvec3(.2126, .7152, .0722)) /
                    density;
            }
            sum += contribution;
            squared += contribution * contribution;
        }
        const double mean = sum / count;
        return std::pair{mean, squared / count - mean * mean};
    };
    const auto [sampled, variance] = estimate(true);
    const auto [cosine, cosine_variance] = estimate(false);
    INFO("importance=" << sampled << " cosine=" << cosine
                       << " variance_ratio=" << variance / cosine_variance);
    REQUIRE(std::abs(sampled - cosine) <
            5 * std::sqrt((variance + cosine_variance) / 60000) + .002);
    REQUIRE(variance < .8 * cosine_variance);
}

TEST_CASE("Cloth material binds surface BSDF and scene UV orientation", "[cloth-bsdf][path]") {
    auto scene = cloth_scene();
    auto& material = scene.materials[0];
    const ClothBsdf reference(material.cloth, material.ior);
    const auto toward_light = -glm::normalize(glm::dvec3(scene.directional_lights[0].direction));
    const auto expected = reference.response({0, 0, 1}, toward_light) * 2.0;
    const auto ray = scene.camera.generate_ray({.5, .5}, 1);
    const auto snapshot = make_snapshot(scene);
    Sampler sampler(91);
    const auto actual = PathTracer(snapshot, {.maximum_depth = 1}).trace(ray, sampler);
    REQUIRE(glm::length(actual - expected) < 1e-10);
    const Bsdf bound(material);
    REQUIRE_FALSE(bound.is_delta());
    REQUIRE(glm::length(bound.evaluate({0, 0, 1}, toward_light) -
                        reference.evaluate({0, 0, 1}, toward_light)) < 1e-12);
    Sampler random(71);
    for (int i = 0; i < 500; ++i) {
        const double component = random.next_1d();
        if (const auto sample = bound.sample({0, 0, 1}, component, random.next_2d())) {
            REQUIRE(sample->measure == SampleMeasure::solid_angle);
            REQUIRE(sample->event == ScatteringEvent::glossy_reflection);
            REQUIRE(glm::length(sample->weight * sample->probability -
                                reference.response({0, 0, 1}, sample->direction)) < 1e-12);
        }
    }
    auto settings = material.cloth->settings();
    settings.rotation_degrees = 90;
    material.cloth = std::make_shared<const ClothAppearance>(settings);
    const ClothBsdf rotated(material.cloth, material.ior);
    Sampler rotated_sampler(91);
    const auto changed =
        PathTracer(make_snapshot(scene), {.maximum_depth = 1}).trace(ray, rotated_sampler);
    REQUIRE(glm::length(changed - rotated.response({0, 0, 1}, toward_light) * 2.0) < 1e-10);
    REQUIRE(glm::length(changed - actual) > .001);
    Sampler old_sampler(91);
    REQUIRE(PathTracer(snapshot, {.maximum_depth = 1}).trace(ray, old_sampler) == actual);
}

TEST_CASE("Cloth scene files preserve curve precision and reject unusable UVs",
          "[cloth-bsdf][io]") {
    auto scene = cloth_scene();
    auto settings = scene.materials[0].cloth->settings();
    settings.rotation_degrees = 17.123456789;
    scene.materials[0].cloth = std::make_shared<const ClothAppearance>(settings);
    TemporaryDirectory directory;
    const auto file = directory.path / std::filesystem::path(u8"布料 材质.json");
    save_scene(scene, file);
    const auto reopened = load_scene(file);
    REQUIRE(reopened.materials[0].kind == MaterialKind::cloth);
    REQUIRE(reopened.materials[0].cloth->settings().rotation_degrees == settings.rotation_degrees);
    REQUIRE(reopened.materials[0].cloth->settings().threads[0].curve.lengths ==
            settings.threads[0].curve.lengths);
    const auto ray = scene.camera.generate_ray({.5, .5}, 1);
    Sampler a(12), b(12);
    REQUIRE(glm::length(PathTracer(make_snapshot(scene)).trace(ray, a) -
                        PathTracer(make_snapshot(reopened)).trace(ray, b)) < 1e-7);
    auto bad = std::make_shared<MeshData>(*scene.meshes[0].data);
    for (auto& face : bad->triangles) {
        for (auto& corner : face.corners) {
            corner.uv = glm::vec2(0);
        }
    }
    scene.meshes[0].data = bad;
    MeshValidationCache cache;
    REQUIRE_THROWS_AS(scene.validate(&cache), std::invalid_argument);
    scene.materials[0].kind = MaterialKind::diffuse;
    REQUIRE_NOTHROW(scene.validate(&cache));
    scene.materials[0].kind = MaterialKind::cloth;
    scene.meshes[0].data = reopened.meshes[0].data;
    REQUIRE_NOTHROW(scene.validate(&cache));
    MeshValidationCache merged;
    merged.merge_validated(cache);
    REQUIRE_NOTHROW(scene.validate(&merged));
}

TEST_CASE("Cloth sampling remains finite at component boundaries and grazing views",
          "[cloth-bsdf]") {
    for (const auto* name : {"linen", "satin"}) {
        const auto preset = make_cloth_preset(name);
        auto settings = preset.appearance->settings();
        settings.coverage = {1, 0};
        const ClothBsdf bsdf(std::make_shared<const ClothAppearance>(settings), preset.ior);
        for (const auto wo : {glm::dvec3(0, 0, 1), glm::normalize(glm::dvec3(1, 0, 1e-9))}) {
            for (const double component : {0.0, .2, .7, std::nextafter(1.0, 0.0)}) {
                for (const auto u :
                     {glm::dvec2(0), glm::dvec2(.5), glm::dvec2(std::nextafter(1.0, 0.0))}) {
                    if (const auto direction = bsdf.sample(wo, component, u)) {
                        const auto value = bsdf.response(wo, *direction);
                        const auto density = bsdf.pdf(wo, *direction);
                        REQUIRE(std::isfinite(density));
                        REQUIRE(density > 0);
                        for (int channel = 0; channel < 3; ++channel) {
                            REQUIRE(std::isfinite(value[channel]));
                            REQUIRE(value[channel] >= 0);
                        }
                    }
                }
            }
        }
    }
}
