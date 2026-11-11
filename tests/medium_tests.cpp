#include <minirender/assets/scene_io.hpp>
#include <minirender/render/medium.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numbers>

using namespace minirender;
using Catch::Approx;
namespace {
constexpr double pi = std::numbers::pi;

MediumRegion box(std::string id, double low, double high) {
    MediumRegion result;
    result.id = std::move(id);
    result.minimum = {low, -1, -1};
    result.maximum = {high, 1, 1};
    result.medium.absorption = {.1, .2, .4};
    return result;
}
void close(glm::dvec3 actual, glm::dvec3 expected, double tolerance = 1e-12) {
    REQUIRE(glm::length(actual - expected) < tolerance);
}
double integrate(const std::function<double(double)>& f) {
    constexpr int n = 200000;
    double sum = f(-1) + f(1);
    for (int i = 1; i < n; ++i) {
        const double c = -1 + 2.0 * i / n;
        sum += (i % 2 == 0 ? 2 : 4) * f(c);
    }
    return sum * (2.0 / n) / 3;
}
class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        path = std::filesystem::temp_directory_path() /
               ("minirender-media-" +
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

TEST_CASE("HG uses outgoing directions and matches analytic angular moments", "[medium][phase]") {
    const glm::dvec3 wo{0, 0, 1};
    REQUIRE(henyey_greenstein(wo, -wo, .7) > henyey_greenstein(wo, wo, .7));
    for (const double g : {-.9, -.4, 0.0, 1e-10, .4, .9}) {
        const auto density = [&](double c) {
            return 2 * pi * henyey_greenstein(wo, {std::sqrt(std::max(0.0, 1 - c * c)), 0, -c}, g);
        };
        REQUIRE(integrate(density) == Approx(1).margin(2e-9));
        REQUIRE(integrate([&](double c) { return c * density(c); }) == Approx(g).margin(2e-9));
        REQUIRE(integrate([&](double c) { return c * c * density(c); }) ==
                Approx((1 + 2 * g * g) / 3).margin(2e-9));
    }
    const auto wi = glm::normalize(glm::dvec3(.3, -.4, .7));
    REQUIRE(henyey_greenstein(wo, wi, .3) == henyey_greenstein(wi, wo, .3));
    REQUIRE(henyey_greenstein(wo, wi, 0) == Approx(1 / (4 * pi)).epsilon(1e-14));
}

TEST_CASE("HG inverse CDF preserves forward mean under arbitrary frames", "[medium][phase]") {
    const auto wo = glm::normalize(glm::dvec3(.3, -.4, .7));
    constexpr int count = 131072;
    for (const double g : {-.97, -.8, -1e-10, 0.0, 1e-10, .8, .97}) {
        glm::dvec3 sum(0);
        double second = 0, length_error = 0, pdf_error = 0;
        for (int i = 0; i < count; ++i) {
            const glm::dvec2 u{(i + .5) / count, std::fmod(i * .6180339887498949, 1)};
            const auto value = sample_henyey_greenstein(wo, g, u);
            const double cosine = -glm::dot(wo, value.direction);
            sum += value.direction;
            second += cosine * cosine;
            length_error = std::max(length_error, std::abs(glm::length(value.direction) - 1));
            pdf_error = std::max(
                pdf_error, std::abs(value.probability - henyey_greenstein(wo, value.direction, g)));
        }
        close(sum / double(count), -g * wo, 6e-5);
        REQUIRE(second / count == Approx((1 + 2 * g * g) / 3).margin(6e-6));
        REQUIRE(length_error < 1e-12);
        REQUIRE(pdf_error == 0);
    }
    for (const double g :
         {std::nextafter(-1.0, 0.0), -1e-16, 0.0, 1e-16, std::nextafter(1.0, 0.0)}) {
        for (const double u : {0.0, .5, std::nextafter(1.0, 0.0)}) {
            const auto value = sample_henyey_greenstein(wo, g, {u, .7});
            REQUIRE(std::isfinite(value.probability));
            REQUIRE(value.probability > 0);
        }
    }
}

TEST_CASE("Medium Beer Lambert handles RGB vacuum zero length and infinity", "[medium]") {
    HomogeneousMedium medium{{.1, .2, 0}, {.3, 0, 0}, 0};
    close(medium_transmittance(medium, 2), {std::exp(-.8), std::exp(-.4), 1});
    close(medium_transmittance(medium, 0), glm::dvec3(1));
    close(medium_transmittance(medium, std::numeric_limits<double>::infinity()), {0, 0, 1});
    close(medium_transmittance({}, std::numeric_limits<double>::infinity()), glm::dvec3(1));
}

TEST_CASE("RGB free flight preserves mixed distance and escape probability", "[medium]") {
    const HomogeneousMedium medium{{.1, .2, .3}, {.3, 0, .9}, .5};
    constexpr int count = 120000;
    constexpr double length = 1.7;
    const auto extinction = medium.extinction();
    const auto tr = medium_transmittance(medium, length);
    const double escape_probability = (tr.x + tr.y + tr.z) / 3;
    Sampler random(719);
    int escaped = 0;
    std::array<int, 4> bins{};
    glm::dvec3 escape_weight(0), scatter_weight(0);
    double largest_weight = 0;
    for (int i = 0; i < count; ++i) {
        const double channel = random.next_1d();
        const double distance = random.next_1d();
        const auto value = sample_medium_distance(medium, length, channel, distance);
        largest_weight = std::max({largest_weight, value.weight.x, value.weight.y, value.weight.z});
        if (value.scattered) {
            ++bins[std::min(std::size_t(value.distance / length * 4), std::size_t(3))];
            scatter_weight += value.weight;
        } else {
            ++escaped;
            escape_weight += value.weight;
        }
    }
    REQUIRE(double(escaped) / count == Approx(escape_probability).margin(.006));
    close(escape_weight / double(count), tr, .012);
    close(scatter_weight / double(count), medium.scattering / extinction * (glm::dvec3(1) - tr),
          .012);
    REQUIRE(largest_weight <= 3 + 1e-12);
    for (int bin = 0; bin < 4; ++bin) {
        const auto a = medium_transmittance(medium, length * bin / 4);
        const auto b = medium_transmittance(medium, length * (bin + 1) / 4);
        const double probability = ((a.x - b.x) + (a.y - b.y) + (a.z - b.z)) / 3;
        REQUIRE(double(bins[std::size_t(bin)]) / count == Approx(probability).margin(.006));
    }
}

TEST_CASE("Free flight retains zero channel escape mass and stable extreme weights", "[medium]") {
    const HomogeneousMedium medium{{0, .2, .3}, {0, .5, .9}, 0};
    const auto zero_channel = sample_medium_distance(medium, 4, .1, .5);
    REQUIRE_FALSE(zero_channel.scattered);
    const auto tr = medium_transmittance(medium, 4);
    close(zero_channel.weight, tr * (3 / (tr.x + tr.y + tr.z)));
    const auto vacuum = sample_medium_distance({}, 4, .5, .9);
    REQUIRE_FALSE(vacuum.scattered);
    close(vacuum.weight, glm::dvec3(1));
    const HomogeneousMedium tiny{{0, 0, 0}, {1e-300, 1e-300, 1e-300}, 0};
    const auto thin = sample_medium_distance(tiny, 1e308, .5, .5);
    REQUIRE(thin.scattered);
    close(thin.weight, glm::dvec3(1), 1e-12);
    const HomogeneousMedium dense{{0, 0, 0}, {1e300, 1e300, 1e300}, 0};
    const auto thick = sample_medium_distance(dense, 1, .5, .5);
    REQUIRE(thick.scattered);
    close(thick.weight, glm::dvec3(1), 1e-12);
}

TEST_CASE("Medium segments sort disjoint regions and clip primary and shadow intervals",
          "[medium]") {
    const std::array regions{box("far", 3, 5), box("near", 0, 1), box("touch", 1, 2)};
    const MediumField field(regions);
    const Ray ray{{-1, 0, 0}, {1, 0, 0}};
    const auto parts = field.segments(ray, {0, 100});
    REQUIRE(parts.count == 3);
    REQUIRE(parts.values[0].region == 1);
    REQUIRE(parts.values[1].region == 2);
    REQUIRE(parts.values[2].region == 0);
    REQUIRE(parts.values[0].interval.minimum == 1);
    REQUIRE(parts.values[2].interval.maximum == 6);
    const auto clipped = field.segments(ray, {1.5, 4.5});
    REQUIRE(clipped.count == 3);
    REQUIRE(clipped.values[0].interval.minimum == 1.5);
    REQUIRE(clipped.values[2].interval.maximum == 4.5);
    close(field.transmittance(ray, {1.5, 4.5}), medium_transmittance(regions[0].medium, 2));
    REQUIRE(field.segments({{.5, 0, 0}, {1, 0, 0}}, {0, .1}).count == 1);
    REQUIRE(field.segments({{0, 1, 0}, {1, 0, 0}}, {0, 100}).count == 0);
    REQUIRE(field.segments({{0, 0, 0}, {-1, 0, 0}}, {0, 100}).count == 0);
    REQUIRE(field.segments(ray, {2, 2}).count == 0);
}

TEST_CASE("Pure absorption and vacuum consume no medium random dimensions", "[medium]") {
    const std::array regions{box("absorber", 0, 1)};
    const MediumField field(regions);
    Sampler first(91), reference(91);
    const auto sampled = field.sample({{-.5, 0, 0}, {1, 0, 0}}, {0, 100}, first);
    REQUIRE_FALSE(sampled.interaction);
    close(sampled.weight, medium_transmittance(regions[0].medium, 1));
    REQUIRE(first.next_u32() == reference.next_u32());
    auto vacuum_regions = regions;
    vacuum_regions[0].medium = {};
    const auto vacuum =
        MediumField(vacuum_regions).sample({{-.5, 0, 0}, {1, 0, 0}}, {0, 100}, first);
    close(vacuum.weight, glm::dvec3(1));
    REQUIRE(first.next_u32() == reference.next_u32());
}

TEST_CASE("Medium validation rejects invalid shapes coefficients overlap and cancellation",
          "[medium]") {
    auto regions = std::vector{box("one", 0, 1), box("two", 2, 3)};
    SECTION("negative coefficient") {
        regions[0].medium.scattering.x = -1;
    }
    SECTION("nonfinite coefficient") {
        regions[0].medium.absorption.y = INFINITY;
    }
    SECTION("overflowing extinction") {
        regions[0].medium.absorption.x = std::numeric_limits<double>::max();
        regions[0].medium.scattering.x = std::numeric_limits<double>::max();
    }
    SECTION("singular box") {
        regions[0].minimum.z = regions[0].maximum.z;
    }
    SECTION("g outside range") {
        regions[0].medium.anisotropy = 1;
    }
    SECTION("duplicate ID") {
        regions[1].id = "one";
    }
    SECTION("overlap") {
        regions[1].minimum.x = .5;
    }
    SECTION("budget") {
        regions.resize(maximum_medium_regions + 1);
    }
    REQUIRE_THROWS_AS(validate_medium_regions(regions), std::invalid_argument);
    REQUIRE_THROWS_AS(sample_henyey_greenstein({0, 0, 1}, 0, {1, 0}), std::invalid_argument);
    REQUIRE_THROWS_AS(henyey_greenstein({0, 0, 2}, {0, 0, 1}, 0), std::invalid_argument);
    REQUIRE_THROWS_AS(medium_transmittance({}, -1), std::invalid_argument);
    CancellationSource stop;
    stop.request();
    REQUIRE_THROWS_AS(MediumField({}, stop.token()), OperationCancelled);
}

TEST_CASE("Medium scene files and snapshots preserve full double parameters", "[medium][io]") {
    SceneDocument scene;
    auto region = box("fog", 0, 1);
    region.name = "彩色雾";
    region.minimum.x = .12345678901234567;
    region.medium.scattering = {.23456789012345678, .3, .7};
    region.medium.anisotropy = .23456789012345678;
    scene.media.push_back(region);
    const auto snapshot = make_snapshot(scene);
    scene.media[0].medium.scattering.x = .9;
    REQUIRE(snapshot->media[0].medium.scattering.x == region.medium.scattering.x);
    TemporaryDirectory directory;
    const auto file = directory.path / std::filesystem::path(u8"介质 场景.json");
    save_scene(scene, file);
    const auto loaded = load_scene(file);
    REQUIRE(loaded.media.size() == 1);
    REQUIRE(loaded.media[0].minimum == scene.media[0].minimum);
    REQUIRE(loaded.media[0].medium.scattering == scene.media[0].medium.scattering);
    REQUIRE(loaded.media[0].medium.anisotropy == region.medium.anisotropy);
    auto json = nlohmann::json::parse(std::ifstream(file));
    json["media"][0]["anisotropy"] = 1;
    std::ofstream(file) << json.dump();
    REQUIRE_THROWS(load_scene(file));
}
