#include <minirender/assets/scene_io.hpp>
#include <minirender/render/light.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <numbers>
#include <stdexcept>

using namespace minirender;
using Catch::Approx;

namespace {

constexpr double pi = std::numbers::pi;

SceneDocument dark_scene() {
    SceneDocument scene;
    scene.environment = glm::vec3(0);
    return scene;
}
void set_map(SceneDocument& scene, Image image) {
    scene.textures.push_back(
        {"sky", {}, ColorEncoding::linear, std::make_shared<const Texture>(std::move(image))});
    scene.environment_map.texture_id = "sky";
}
glm::dvec3 latlong(double u, double v) {
    return {std::sin(v * pi) * std::cos(u * 2 * pi), std::cos(v * pi),
            std::sin(v * pi) * std::sin(u * 2 * pi)};
}

SceneDocument area_scene() {
    auto scene = dark_scene();
    MeshData mesh;
    mesh.positions = {{-1, -1, 0}, {1, -1, 0}, {-1, 1, 0}};
    mesh.triangles.push_back({{{{0, {0, 0, 1}, {}}, {1, {0, 0, 1}, {}}, {2, {0, 0, 1}, {}}}}});
    mesh.validate();
    scene.meshes.push_back({"mesh", {}, std::make_shared<const MeshData>(mesh)});
    scene.materials.push_back({"lamp", "lamp"});
    scene.materials[0].emission = {4, 2, 1};
    scene.objects.push_back({"lamp", "lamp", "mesh", "lamp", {}, true});
    return scene;
}

class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root =
            std::filesystem::temp_directory_path() / ("minirender-light-" + std::to_string(stamp));
        std::filesystem::create_directories(root);
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    std::filesystem::path root;
};

} // namespace

TEST_CASE("Constant environment maps sample uniform solid angle including pole limits", "[light]") {
    auto scene = dark_scene();
    scene.environment = glm::vec3(0.1F);
    scene.environment_map.strength = 2;
    Image image(8, 4);
    for (std::size_t y = 0; y < image.height(); ++y) {
        for (std::size_t x = 0; x < image.width(); ++x) {
            image.at(x, y) = {2, 1, 0.5F};
        }
    }
    set_map(scene, std::move(image));
    const EnvironmentLight environment(*make_snapshot(scene));
    const glm::dvec3 expected = glm::dvec3(scene.environment) + glm::dvec3(4, 2, 1);
    const double power = glm::dot(expected, glm::dvec3(0.2126, 0.7152, 0.0722)) * 4 * pi;
    REQUIRE(environment.luminance_integral() == Approx(power).epsilon(1e-12));
    for (const auto direction : {glm::dvec3(0, 1, 0), glm::dvec3(0, -1, 0), latlong(0.37, 0.2)}) {
        REQUIRE(glm::length(environment.evaluate(direction) - expected) < 1e-12);
        REQUIRE(environment.pdf(direction) == Approx(1 / (4 * pi)).epsilon(1e-12));
    }
    for (const auto u :
         {glm::dvec2(0), glm::dvec2(0.05, 0), glm::dvec2(std::nextafter(1.0, 0.0))}) {
        const auto sample = environment.sample(u);
        REQUIRE(glm::length(sample.direction) == Approx(1).margin(1e-12));
        REQUIRE(std::isfinite(sample.probability));
        REQUIRE(sample.probability > 0);
    }
    Sampler sampler(43);
    glm::dvec3 average(0);
    for (int index = 0; index < 10000; ++index) {
        const auto sample = environment.sample(sampler.next_2d());
        REQUIRE(glm::length(sample.direction) == Approx(1).margin(1e-12));
        REQUIRE(sample.probability == Approx(environment.pdf(sample.direction)).epsilon(1e-12));
        average += sample.direction;
    }
    REQUIRE(glm::length(average / 10000.0) < 0.025);
}

TEST_CASE("Environment importance PDFs normalize and retain bilinear support", "[light]") {
    auto scene = dark_scene();
    Image image(8, 4);
    image.at(3, 1) = glm::vec3(100);
    set_map(scene, std::move(image));
    const EnvironmentLight environment(*make_snapshot(scene));
    double integral = 0;
    for (int y = 0; y < 4; ++y) {
        const double angle = (2 * pi / 8) * (std::cos(y * pi / 4) - std::cos((y + 1) * pi / 4));
        for (int x = 0; x < 8; ++x) {
            integral += environment.pdf(latlong((x + 0.5) / 8, (y + 0.5) / 4)) * angle;
        }
    }
    REQUIRE(integral == Approx(1).margin(1e-12));
    const auto neighbor = latlong(4.1 / 8, 1.5 / 4);
    REQUIRE(environment.evaluate(neighbor).x == Approx(40).margin(1e-10));
    REQUIRE(environment.pdf(neighbor) == Approx(0.05 / (4 * pi)).epsilon(1e-12));
    Sampler sampler(78);
    std::size_t bright_cell = 0;
    for (int index = 0; index < 20000; ++index) {
        const auto sample = environment.sample(sampler.next_2d());
        REQUIRE(sample.probability == Approx(environment.pdf(sample.direction)).epsilon(1e-12));
        REQUIRE(glm::length(sample.radiance - environment.evaluate(sample.direction)) < 1e-12);
        double u = std::atan2(sample.direction.z, sample.direction.x) / (2 * pi);
        u -= std::floor(u);
        const double v =
            std::atan2(std::hypot(sample.direction.x, sample.direction.z), sample.direction.y) / pi;
        bright_cell += static_cast<int>(u * 8) == 3 && static_cast<int>(v * 4) == 1;
    }
    const double solid_angle = (2 * pi / 8) * (std::cos(pi / 4) - std::cos(pi / 2));
    const double expected_mass = 0.95 + 0.05 * solid_angle / (4 * pi);
    const double tolerance = 5 * std::sqrt(20000 * expected_mass * (1 - expected_mass));
    REQUIRE(std::abs(static_cast<double>(bright_cell) - 20000 * expected_mass) < tolerance);
    scene.environment_map.rotation_degrees = 90;
    const EnvironmentLight rotated(*make_snapshot(scene));
    const auto local = latlong(3.5 / 8, 1.5 / 4);
    const glm::dvec3 world{local.z, local.y, -local.x};
    REQUIRE(glm::length(rotated.evaluate(world) - environment.evaluate(local)) < 1e-10);
    REQUIRE(rotated.pdf(world) == Approx(environment.pdf(local)).epsilon(1e-12));
}

TEST_CASE("Negative HDR samples cannot erase a still-emitting environment during mip selection",
          "[light]") {
    auto scene = dark_scene();
    Image image(2048, 1);
    for (std::size_t x = 0; x < image.width(); ++x) {
        image.at(x, 0) = glm::vec3(x % 2 == 0 ? 1 : -1);
    }
    set_map(scene, std::move(image));
    REQUIRE(scene.textures[0].data->level(1).at(0, 0) == glm::vec3(0));
    const LightSampler lights(make_snapshot(scene));
    REQUIRE(lights.size() == 1);
    REQUIRE(lights.environment().luminance_integral() == Approx(2 * pi).epsilon(1e-12));
    REQUIRE(lights.environment().evaluate(latlong(0.5 / 2048, 0.5)).x == Approx(1).margin(1e-10));
    REQUIRE(lights.environment().pdf({1, 0, 0}) == Approx(1 / (4 * pi)).epsilon(1e-12));
}

TEST_CASE("Area-light sampling uses transformed area and the solid-angle Jacobian", "[light]") {
    const auto snapshot = make_snapshot(area_scene());
    const LightSampler lights(snapshot);
    const SceneBvh bvh(snapshot);
    REQUIRE(lights.size() == 1);
    const glm::dvec3 reference{0, 0, 2};
    const auto sample = lights.sample(reference, 0.5, {0.25, 0.5});
    REQUIRE(sample);
    REQUIRE(sample->kind == LightKind::area);
    REQUIRE(sample->measure == SampleMeasure::solid_angle);
    REQUIRE(sample->distance == Approx(std::sqrt(4.5)));
    REQUIRE(sample->probability == Approx(std::pow(4.5, 1.5) / 4));
    REQUIRE(sample->radiance == glm::dvec3(4, 2, 1));
    const auto below = lights.sample({0, 0, -2}, 0.5, {0.25, 0.5});
    REQUIRE(below);
    REQUIRE(below->probability == Approx(sample->probability));
    Sampler random(4);
    for (int index = 0; index < 1000; ++index) {
        const double light_u = random.next_1d();
        const auto surface_u = random.next_2d();
        const auto selected = lights.sample(reference, light_u, surface_u);
        REQUIRE(selected);
        const auto hit = bvh.intersect({glm::vec3(reference), glm::vec3(selected->direction)});
        REQUIRE(hit);
        REQUIRE(lights.area_pdf(reference, *hit) == Approx(selected->probability).epsilon(2e-6));
    }
}

TEST_CASE("Light selection includes area, directional and environment probabilities", "[light]") {
    auto scene = area_scene();
    auto second = scene.objects[0];
    second.id = "second";
    second.transform.translation.x = 3;
    second.transform.scale = {2, 2, 1};
    scene.objects.push_back(second);
    scene.directional_lights.push_back({"sun", {0, -2, 0}, {1, 1, 1}, 2});
    scene.environment = glm::vec3(0.25F);
    const LightSampler lights(make_snapshot(scene));
    const double le = glm::dot(glm::dvec3(4, 2, 1), glm::dvec3(0.2126, 0.7152, 0.0722));
    const std::array powers{4 * pi * le, 16 * pi * le, 26 * pi, 13 * pi * pi};
    double total = 0;
    for (const auto power : powers) {
        total += power;
    }
    REQUIRE(lights.size() == 4);
    double cdf = 0;
    for (std::size_t index = 0; index < powers.size(); ++index) {
        const double mass = powers[index] / total;
        REQUIRE(lights.selection_probability(index) == Approx(mass).epsilon(1e-12));
        const auto sample = lights.sample({0, 0, 2}, cdf + mass * 0.5, {0.3, 0.4});
        REQUIRE(sample);
        REQUIRE(sample->emitter_index == index);
        if (index == 2) {
            REQUIRE(sample->measure == SampleMeasure::discrete);
            REQUIRE(sample->direction == glm::dvec3(0, 1, 0));
            REQUIRE(sample->radiance == glm::dvec3(2));
            REQUIRE(sample->probability == Approx(mass));
        }
        if (index == 3) {
            REQUIRE(sample->kind == LightKind::environment);
            REQUIRE(sample->probability == Approx(mass / (4 * pi)));
            REQUIRE(sample->probability == lights.environment_pdf(sample->direction));
        }
        cdf += mass;
    }
    const LightSampler empty(make_snapshot(dark_scene()));
    REQUIRE(empty.size() == 0);
    REQUIRE_FALSE(empty.sample({0, 0, 0}, 0.4, {0.5, 0.5}));
    REQUIRE(empty.environment_pdf({0, 1, 0}) == 0);
    REQUIRE_THROWS_AS(LightSampler(nullptr), std::invalid_argument);
}

TEST_CASE("Environment asset references round trip and freeze with scene snapshots",
          "[light][scene]") {
    TemporaryDirectory directory;
    auto scene = dark_scene();
    const auto source =
        std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes/assets/textures/radiance.hdr";
    scene.textures.push_back(
        {"sky", source, ColorEncoding::linear,
         std::make_shared<const Texture>(Image::read(source, ColorEncoding::linear))});
    scene.environment_map = {"sky", 2, 90};
    const auto snapshot = make_snapshot(scene);
    const auto file = directory.root / std::filesystem::path(std::u8string(u8"环境 空格.json"));
    save_scene(scene, file);
    const auto reloaded = load_scene(file);
    REQUIRE(reloaded.environment_map.texture_id == "sky");
    REQUIRE(reloaded.environment_map.strength == 2);
    REQUIRE(reloaded.environment_map.rotation_degrees == 90);
    REQUIRE(make_snapshot(reloaded)->environment_texture != nullptr);
    REQUIRE(snapshot->environment_texture == scene.textures[0].data);
    scene.environment_map.strength = 10;
    REQUIRE(snapshot->environment_strength == 2);
    scene.environment_map.texture_id = "missing";
    REQUIRE_THROWS(make_snapshot(scene));
    scene.environment_map.texture_id = "sky";
    scene.environment_map.strength = -1;
    REQUIRE_THROWS(make_snapshot(scene));
}
