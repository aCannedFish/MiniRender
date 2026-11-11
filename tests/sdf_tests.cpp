#include <minirender/assets/sdf_io.hpp>
#include <minirender/core/sampling.hpp>
#include <minirender/render/sdf_render.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <fstream>

using namespace minirender;
namespace {
SdfDocument sphere(float radius = 1) {
    SdfDocument scene;
    scene.nodes.push_back({"sphere", "球", SdfKind::sphere, {radius, 0, 0}});
    return scene;
}
class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        path = std::filesystem::temp_directory_path() /
               ("minirender-sdf-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    std::filesystem::path path;
};
std::string bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
} // namespace

TEST_CASE("SDF primitive distances agree with analytic geometry", "[sdf]") {
    auto scene = sphere();
    SdfField field(scene);
    REQUIRE(field.sample({0, 0, 0}).distance == -1.0);
    REQUIRE(field.sample({0, 0, 2}).distance == 1.0);
    REQUIRE(std::abs(field.sample({3, 4, 0}).distance - 4) < 1e-14);
    scene.nodes[0].kind = SdfKind::box;
    scene.nodes[0].parameters = {1, 2, 3};
    field = SdfField(scene);
    REQUIRE(field.sample({0, 0, 0}).distance == -1.0);
    REQUIRE(field.sample({2, 3, 3}).distance == std::sqrt(2.0));
    REQUIRE(field.sample({1, 1, 1}).distance == 0.0);
    scene.nodes[0].kind = SdfKind::torus;
    scene.nodes[0].parameters = {1, .25F, 0};
    field = SdfField(scene);
    REQUIRE(field.sample({0, 0, 0}).distance == .75);
    REQUIRE(field.sample({1, 0, 0}).distance == -.25);
    REQUIRE(field.sample({1.25, 0, 0}).distance == 0.0);
    REQUIRE(field.sample({0, .5, 1}).distance == .25);
}

TEST_CASE("SDF demo cutter removes the spherical cap with clearance", "[sdf]") {
    const auto scene = make_sdf_example();
    const auto& ball = scene.nodes[0];
    const glm::dvec3 top = glm::dvec3(ball.translation) + glm::dvec3(0, ball.parameters.x, 0);
    REQUIRE(SdfField(scene).sample(top).distance > .05);
    REQUIRE(SdfField(scene).sample(top + glm::dvec3(0, 1e-4, 0)).distance > .05);
}

TEST_CASE("SDF rigid transforms and uniform scale preserve distance units", "[sdf]") {
    auto scene = sphere();
    scene.nodes[0].translation = {2, -1, 3};
    scene.nodes[0].rotation_degrees = {0, 0, 90};
    scene.nodes[0].scale = 2;
    SdfField field(scene);
    REQUIRE(std::abs(field.sample({2, -1, 6}).distance - 1) < 1e-12);
    scene.nodes[0].kind = SdfKind::box;
    scene.nodes[0].parameters = {1, .5F, .25F};
    field = SdfField(scene);
    REQUIRE(std::abs(field.sample({2, 2, 3}).distance - 1) < 1e-12);
    REQUIRE(glm::length(field.normal({2, 1, 3}, 1e-4) - glm::dvec3(0, 1, 0)) < 1e-9);
    REQUIRE(field.bounds().minimum.x <= 1.0);
    REQUIRE(field.bounds().maximum.x >= 3.0);
    REQUIRE(field.bounds().minimum.y <= -3.0);
    REQUIRE(field.bounds().maximum.y >= 1.0);
}

TEST_CASE("SDF CSG compiles arbitrary DAG order and exposes cut material", "[sdf]") {
    auto scene = sphere();
    scene.nodes.push_back({"cutter", "切割球", SdfKind::sphere, {.7F, 0, 0}, {.5F, 0, 0}});
    scene.nodes.push_back({"root", "差集", SdfKind::subtract, {}, {}, {}, 1, {}, 0, 1});
    scene.root = 2;
    SdfField field(scene);
    const auto cut = trace_sdf(field, {{3, 0, 0}, {-1, 0, 0}});
    REQUIRE(cut.state == SdfTraceState::hit);
    REQUIRE(std::abs(cut.position.x + .2) < 2e-4);
    REQUIRE(field.nodes()[cut.material].source_index == 1);
    REQUIRE(glm::length(field.normal(cut.position, 1e-4) - glm::dvec3(1, 0, 0)) < 1e-7);
    std::swap(scene.nodes[0], scene.nodes[2]);
    scene.nodes[0].left = 2;
    scene.nodes[0].right = 1;
    scene.root = 0;
    const SdfField reordered(scene);
    REQUIRE(reordered.nodes().size() == 3);
    REQUIRE(std::abs(reordered.sample({-.5, 0, 0}).distance - field.sample({-.5, 0, 0}).distance) <
            1e-14);
    scene.nodes[0].kind = SdfKind::unite;
    REQUIRE(SdfField(scene).sample({0, 0, 0}).distance == -1.0);
    scene.nodes[0].kind = SdfKind::intersect;
    REQUIRE(SdfField(scene).sample({-.8, 0, 0}).distance > 0);
}

TEST_CASE("SDF sphere tracing agrees with independent quadratic intersections", "[sdf]") {
    auto scene = sphere();
    scene.nodes[0].translation = {.3F, .5F, -.2F};
    scene.nodes[0].scale = 1.7F;
    const auto center = glm::dvec3(scene.nodes[0].translation);
    const double radius = scene.nodes[0].scale;
    const SdfField field(scene);
    Sampler sampler(101);
    std::size_t hits = 0;
    std::size_t misses = 0;
    for (int i = 0; i < 500; ++i) {
        const Ray ray{
            {0, 0, 5},
            glm::normalize(glm::dvec3(2 * sampler.next_1d() - 1, 2 * sampler.next_1d() - 1, -1))};
        const auto delta = ray.origin - center;
        const double b = glm::dot(delta, ray.direction);
        const double discriminant = b * b - glm::dot(delta, delta) + radius * radius;
        if (std::abs(discriminant) < .05) {
            continue;
        }
        const auto result = trace_sdf(field, ray);
        if (discriminant < 0) {
            REQUIRE(result.state == SdfTraceState::miss);
            ++misses;
        } else {
            const double t = -b - std::sqrt(discriminant);
            REQUIRE(result.state == SdfTraceState::hit);
            REQUIRE(std::abs(result.distance - t) < .002);
            REQUIRE(result.residual < 2e-4);
            ++hits;
        }
    }
    REQUIRE(hits > 20);
    REQUIRE(misses > 100);
    const auto inside = trace_sdf(field, {center, {1, 0, 0}});
    REQUIRE(inside.state == SdfTraceState::hit);
    REQUIRE(std::abs(inside.distance - radius) < 2e-4);
}

TEST_CASE("SDF tracing distinguishes limits clipped misses and torus holes", "[sdf]") {
    auto scene = sphere();
    SdfField field(scene);
    REQUIRE(trace_sdf(field, {{0, 0, 4}, {0, 0, -1}}, {}, 0, 1).state == SdfTraceState::miss);
    SdfTraceSettings settings;
    settings.maximum_steps = 1;
    REQUIRE(trace_sdf(field, {{0, 0, 4}, {0, 0, -1}}, settings).state == SdfTraceState::step_limit);
    REQUIRE(trace_sdf(field, {{2, 0, 0}, {0, 0, 1}}).steps == 0);
    scene.nodes[0].kind = SdfKind::torus;
    scene.nodes[0].parameters = {1, .25F, 0};
    field = SdfField(scene);
    REQUIRE(trace_sdf(field, {{0, 3, 0}, {0, -1, 0}}).state == SdfTraceState::miss);
    const auto hit = trace_sdf(field, {{1, 3, 0}, {0, -1, 0}});
    REQUIRE(hit.state == SdfTraceState::hit);
    REQUIRE(std::abs(hit.distance - 2.75) < 2e-4);
    REQUIRE(glm::length(field.normal(hit.position, 1e-4) - glm::dvec3(0, 1, 0)) < 1e-6);
}

TEST_CASE("SDF validation rejects cyclic graphs and preserves cancellation", "[sdf]") {
    auto scene = make_sdf_example();
    scene.nodes[6].left = 6;
    REQUIRE_THROWS_AS(SdfField(scene), std::invalid_argument);
    scene = make_sdf_example();
    scene.nodes[0].scale = 0;
    REQUIRE_THROWS_AS(SdfField(scene), std::invalid_argument);
    scene = make_sdf_example();
    scene.nodes[0].id = scene.nodes[1].id;
    REQUIRE_THROWS_AS(SdfField(scene), std::invalid_argument);
    scene = make_sdf_example();
    scene.nodes[1].left = 0;
    REQUIRE_THROWS_AS(SdfField(scene), std::invalid_argument);
    CancellationSource cancellation;
    cancellation.request();
    REQUIRE_THROWS_AS(SdfField(make_sdf_example(), cancellation.token()), OperationCancelled);
    const SdfField field(sphere());
    REQUIRE_THROWS_AS(trace_sdf(field, {{0, 0, 4}, {0, 0, -1}}, {}, 0, 10, cancellation.token()),
                      OperationCancelled);
    REQUIRE_THROWS_AS(trace_sdf(field, {{0, 0, 4}, {0, 0, -2}}), std::invalid_argument);
}

TEST_CASE("SDF JSON keeps graph transforms and Unicode file paths", "[sdf][io]") {
    TemporaryDirectory directory;
    const auto path = directory.path / std::filesystem::path(u8"距离场 场景.json");
    auto original = make_sdf_example();
    original.camera.aperture_radius = .03F;
    original.camera.focus_distance = 6;
    save_sdf(original, path);
    const auto loaded = load_sdf(path);
    REQUIRE(loaded.nodes.size() == original.nodes.size());
    REQUIRE(loaded.nodes[2].left == 0);
    REQUIRE(loaded.nodes[2].right == 1);
    REQUIRE(loaded.camera.position == original.camera.position);
    REQUIRE(loaded.camera.aperture_radius == original.camera.aperture_radius);
    REQUIRE(loaded.camera.focus_distance == original.camera.focus_distance);
    const SdfField a(original);
    const SdfField b(loaded);
    for (const auto point : {glm::dvec3(0), glm::dvec3(.5, .8, 0), glm::dvec3(2, 1, 3)}) {
        REQUIRE(a.sample(point).distance == b.sample(point).distance);
    }
    const auto saved = bytes(path);
    auto invalid = original;
    invalid.root = 99;
    REQUIRE_THROWS_AS(save_sdf(invalid, path), std::invalid_argument);
    REQUIRE(bytes(path) == saved);
    CancellationSource cancellation;
    cancellation.request();
    REQUIRE_THROWS_AS(save_sdf(original, path, cancellation.token()), OperationCancelled);
    REQUIRE(bytes(path) == saved);
}

TEST_CASE("SDF CPU normal depth and surface views match an analytic sphere", "[sdf][render]") {
    auto scene = sphere();
    scene.camera.position = {0, 0, 4};
    scene.camera.target = {0, 0, 0};
    scene.light.direction = {0, 0, -1};
    const auto normal = render_sdf(scene, {21, 21, SdfView::normals});
    const auto depth = render_sdf(scene, {21, 21, SdfView::depth});
    const auto surface = render_sdf(scene, {21, 21, SdfView::surface});
    const auto steps = render_sdf(scene, {21, 21, SdfView::steps});
    REQUIRE(normal.hits > 50);
    REQUIRE(normal.hits == depth.hits);
    REQUIRE(normal.hits == steps.hits);
    REQUIRE(normal.limited == 0);
    REQUIRE(normal.normal_fallbacks == 0);
    std::size_t probes = 0;
    for (std::size_t y = 0; y < 21; ++y) {
        for (std::size_t x = 0; x < 21; ++x) {
            const auto ray = scene.camera.generate_ray(
                {(static_cast<double>(x) + .5) / 21, (static_cast<double>(y) + .5) / 21}, 1);
            const double b = glm::dot(ray.origin, ray.direction);
            const double discriminant = b * b - glm::dot(ray.origin, ray.origin) + 1;
            if (discriminant < .1) {
                continue;
            }
            const double distance = -b - std::sqrt(discriminant);
            const auto n = glm::normalize(ray.origin + ray.direction * distance);
            const auto expected_normal = glm::vec3(n * .5 + .5);
            const auto expected_depth =
                glm::vec3(static_cast<float>(1 / (1 - distance * ray.direction.z)));
            const auto expected_surface =
                scene.nodes[0].color *
                (scene.ambient + scene.light.color * scene.light.intensity *
                                     static_cast<float>(n.z / 3.14159265358979323846));
            REQUIRE(glm::length(normal.image.at(x, y) - expected_normal) < .001F);
            REQUIRE(glm::length(depth.image.at(x, y) - expected_depth) < .0001F);
            REQUIRE(glm::length(surface.image.at(x, y) - expected_surface) < .001F);
            ++probes;
        }
    }
    REQUIRE(probes > 50);
    scene.trace.maximum_steps = 1;
    const auto limited = render_sdf(scene, {21, 21, SdfView::steps});
    REQUIRE(limited.limited > 50);
    REQUIRE(limited.image.at(10, 10) == glm::vec3(1, 0, 1));
}

TEST_CASE("SDF CPU shadows change occluded surfaces and cancellation stops rendering", "[sdf]") {
    const auto scene = make_sdf_example();
    const auto shadowed = render_sdf(scene, {48, 48, SdfView::surface, true});
    const auto unshadowed = render_sdf(scene, {48, 48, SdfView::surface, false});
    REQUIRE(shadowed.hits == unshadowed.hits);
    REQUIRE(shadowed.limited == 0);
    std::size_t occluded = 0;
    for (std::size_t y = 0; y < 48; ++y) {
        for (std::size_t x = 0; x < 48; ++x) {
            const auto difference = unshadowed.image.at(x, y) - shadowed.image.at(x, y);
            REQUIRE(glm::all(glm::greaterThanEqual(difference, glm::vec3(-1e-6F))));
            occluded += glm::length(difference) > .1F ? 1 : 0;
        }
    }
    REQUIRE(occluded > 10);
    CancellationSource source;
    std::size_t last = 0;
    REQUIRE_THROWS_AS(render_sdf(scene, {48, 48}, source.token(),
                                 [&](std::size_t completed, std::size_t total) {
                                     REQUIRE(total == 48 * 48);
                                     REQUIRE(completed >= last);
                                     last = completed;
                                     if (completed >= 256) {
                                         source.request();
                                     }
                                 }),
                      OperationCancelled);
    REQUIRE(last == 256);
}

TEST_CASE("SDF JSON rejects fractional and overflowing step counts", "[sdf][io]") {
    TemporaryDirectory directory;
    const auto path = directory.path / "scene.json";
    save_sdf(make_sdf_example(), path);
    const auto saved = bytes(path);
    const std::string key = "\"maximum_steps\": 512";
    const auto offset = saved.find(key);
    REQUIRE(offset != std::string::npos);
    for (const auto* value : {"1.5", "-1", "4294967297", "0", "true"}) {
        auto malformed = saved;
        malformed.replace(offset, key.size(), std::string("\"maximum_steps\": ") + value);
        {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream << malformed;
        }
        REQUIRE_THROWS_AS(load_sdf(path), std::invalid_argument);
    }
}
