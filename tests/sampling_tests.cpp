#include <minirender/core/sampling.hpp>
#include <minirender/render/bsdf.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <future>
#include <limits>
#include <numbers>
#include <stdexcept>

using namespace minirender;
using Catch::Approx;

namespace {

constexpr double pi = std::numbers::pi;

Material material(MaterialKind kind, float roughness = 0.5F) {
    Material value;
    value.kind = kind;
    value.base_color = glm::vec3(1);
    value.roughness = roughness;
    return value;
}

void check_sample(const BsdfSample& sample) {
    REQUIRE(glm::length(sample.direction) == Approx(1).margin(1e-10));
    REQUIRE(std::isfinite(sample.probability));
    REQUIRE(sample.probability > 0);
    for (int channel = 0; channel < 3; ++channel) {
        REQUIRE(std::isfinite(sample.weight[channel]));
        REQUIRE(sample.weight[channel] >= 0);
    }
}

// 显式分配随机维度，避免函数实参求值顺序在编译器之间改变随机流的用途。
std::optional<BsdfSample> next_sample(const Bsdf& bsdf, glm::dvec3 wo, Sampler& sampler) {
    const double component = sampler.next_1d();
    const auto direction = sampler.next_2d();
    return bsdf.sample(wo, component, direction);
}

// 五个标准差加固定舍入余量，按预先给定的分布检查频次，不从样本拟合期望值。
void check_frequency(std::size_t count, double probability, std::size_t total) {
    const double expected = probability * static_cast<double>(total);
    const double tolerance = 5 * std::sqrt(expected * (1 - probability)) + 8;
    REQUIRE(std::abs(static_cast<double>(count) - expected) < tolerance);
}

} // namespace

TEST_CASE("PCG follows its reference sequence and sample streams ignore scheduling", "[sampling]") {
    Sampler reference(42, 54);
    for (const std::uint32_t expected :
         {0xa15c02b7U, 0x7b47f409U, 0xba1d3330U, 0x83d2f293U, 0xbfa4784bU, 0xcbed606eU}) {
        REQUIRE(reference.next_u32() == expected);
    }
    const auto values = [](std::uint64_t pixel) {
        auto sampler = Sampler::for_sample(12, pixel, 7);
        std::array<double, 32> result{};
        for (auto& value : result) {
            value = sampler.next_1d();
        }
        return result;
    };
    std::vector<std::future<std::array<double, 32>>> futures;
    for (std::uint64_t pixel = 0; pixel < 4; ++pixel) {
        futures.push_back(std::async(std::launch::async, values, pixel));
    }
    for (std::uint64_t pixel = 0; pixel < 4; ++pixel) {
        REQUIRE(futures[pixel].get() == values(pixel));
    }
    REQUIRE(values(0) != values(1));
    REQUIRE(Sampler::for_sample(12, 0, 7).next_u32() != Sampler::for_sample(12, 0, 8).next_u32());
    REQUIRE(Sampler::for_sample(12, 0, 7).next_u32() != Sampler::for_sample(13, 0, 7).next_u32());
}

TEST_CASE("Geometric samplers match analytic moments and bins", "[sampling]") {
    constexpr std::size_t count = 100000;
    Sampler sampler(914, 21);
    glm::dvec3 sphere_sum(0), triangle_sum(0);
    double cosine_sum = 0, disk_radius_sum = 0, triangle_cross_sum = 0;
    std::array<std::size_t, 4> disk_bins{}, sphere_bins{}, cosine_bins{};
    for (std::size_t index = 0; index < count; ++index) {
        const auto u = sampler.next_2d();
        REQUIRE(u.x >= 0);
        REQUIRE(u.x < 1);
        REQUIRE(u.y >= 0);
        REQUIRE(u.y < 1);
        const auto disk = sample_disk(u);
        const double radius2 = glm::dot(disk, disk);
        REQUIRE(radius2 <= 1.0 + 1e-12);
        disk_radius_sum += radius2;
        ++disk_bins[std::min(static_cast<std::size_t>(radius2 * 4), std::size_t(3))];
        const auto sphere = sample_sphere(u);
        REQUIRE(glm::length(sphere) == Approx(1).margin(1e-12));
        sphere_sum += sphere;
        ++sphere_bins[std::min(static_cast<std::size_t>((sphere.z + 1) * 2), std::size_t(3))];
        const auto cosine = sample_cosine_hemisphere(u);
        REQUIRE(cosine.z >= 0);
        REQUIRE(glm::length(cosine) == Approx(1).margin(1e-12));
        cosine_sum += cosine.z;
        ++cosine_bins[std::min(static_cast<std::size_t>(cosine.z * 4), std::size_t(3))];
        const auto triangle = sample_triangle(u);
        REQUIRE(triangle.x + triangle.y + triangle.z == Approx(1).margin(1e-12));
        REQUIRE(glm::all(glm::greaterThanEqual(triangle, glm::dvec3(0))));
        triangle_sum += triangle;
        triangle_cross_sum += triangle.x * triangle.y;
    }
    for (std::size_t bin = 0; bin < 4; ++bin) {
        check_frequency(disk_bins[bin], 0.25, count);
        check_frequency(sphere_bins[bin], 0.25, count);
        const double low = static_cast<double>(bin) / 4;
        const double high = static_cast<double>(bin + 1) / 4;
        check_frequency(cosine_bins[bin], high * high - low * low, count);
    }
    REQUIRE(glm::length(sphere_sum / static_cast<double>(count)) < 0.01);
    REQUIRE(disk_radius_sum / count == Approx(0.5).margin(0.006));
    REQUIRE(cosine_sum / count == Approx(2.0 / 3).margin(0.006));
    for (int channel = 0; channel < 3; ++channel) {
        REQUIRE(triangle_sum[channel] / count == Approx(1.0 / 3).margin(0.006));
    }
    REQUIRE(triangle_cross_sum / count == Approx(1.0 / 12).margin(0.002));
    REQUIRE(sample_disk({0.5, 0.5}) == glm::dvec2(0));
    REQUIRE_THROWS_AS(sample_sphere({1, 0}), std::invalid_argument);
    REQUIRE_THROWS_AS(sample_disk({0, -0.1}), std::invalid_argument);
    REQUIRE_THROWS_AS(sample_triangle({std::numeric_limits<double>::quiet_NaN(), 0}),
                      std::invalid_argument);
}

TEST_CASE("Discrete distributions preserve mass, empty cases and zero-weight intervals",
          "[sampling]") {
    const std::array weights{0.0, 1.0, 3.0, 0.0};
    const DiscreteDistribution distribution(weights);
    REQUIRE(distribution.size() == 4);
    REQUIRE(distribution.probability(0) == 0);
    REQUIRE(distribution.probability(1) == Approx(0.25));
    REQUIRE(distribution.probability(2) == Approx(0.75));
    REQUIRE(distribution.probability(3) == 0);
    REQUIRE(distribution.sample(0)->index == 1);
    REQUIRE(distribution.sample(0.25)->index == 2);
    REQUIRE(distribution.sample(std::nextafter(1.0, 0.0))->index == 2);
    Sampler sampler(41);
    std::size_t selected = 0;
    for (std::size_t index = 0; index < 100000; ++index) {
        const auto sample = distribution.sample(sampler.next_1d());
        REQUIRE(sample);
        REQUIRE(sample->probability == distribution.probability(sample->index));
        REQUIRE(sample->remapped >= 0);
        REQUIRE(sample->remapped < 1);
        selected += sample->index == 1;
    }
    check_frequency(selected, 0.25, 100000);
    const std::array zeros{0.0, 0.0, 0.0};
    const DiscreteDistribution uniform(zeros);
    REQUIRE(uniform.probability(1) == Approx(1.0 / 3));
    REQUIRE_FALSE(DiscreteDistribution().sample(0));
    const std::array large{1e308, 1e308};
    REQUIRE(DiscreteDistribution(large).probability(0) == Approx(0.5));
    const std::array invalid{-1.0};
    REQUIRE_THROWS_AS(DiscreteDistribution(invalid), std::invalid_argument);
    REQUIRE_THROWS_AS(distribution.sample(1), std::invalid_argument);
    REQUIRE_THROWS_AS(distribution.probability(4), std::out_of_range);
}

TEST_CASE("Shading frames are right handed and invertible at normal poles", "[bsdf]") {
    for (const auto normal : {glm::dvec3(0, 0, 1), glm::dvec3(0, 0, -1), glm::dvec3(0.1, 1, 0.001),
                              glm::dvec3(1e200), glm::dvec3(1e-200)}) {
        const ShadingFrame frame(normal);
        const auto x = frame.to_world({1, 0, 0});
        const auto y = frame.to_world({0, 1, 0});
        REQUIRE(glm::dot(glm::cross(x, y), frame.normal()) == Approx(1).margin(1e-12));
        const auto value = glm::normalize(glm::dvec3(0.4, 0.5, 0.6));
        REQUIRE(glm::length(frame.to_local(frame.to_world(value)) - value) < 1e-12);
    }
    REQUIRE_THROWS_AS(ShadingFrame(glm::dvec3(0)), std::invalid_argument);
}

TEST_CASE("Lambert sampling agrees with eval, PDF and directional albedo", "[bsdf]") {
    auto surface = material(MaterialKind::diffuse);
    surface.base_color = {0.8F, 0.4F, 0.2F};
    const Bsdf bsdf(surface, true, {0.5F, 2, -1});
    const glm::dvec3 expected{static_cast<double>(surface.base_color.r) * 0.5, surface.base_color.g,
                              0};
    Sampler sampler(4);
    const auto wo = glm::normalize(glm::dvec3(0.5, 0.1, 1));
    double pdf_integral = 0;
    for (int index = 0; index < 5000; ++index) {
        const auto sample = next_sample(bsdf, wo, sampler);
        REQUIRE(sample);
        check_sample(*sample);
        REQUIRE(sample->measure == SampleMeasure::solid_angle);
        REQUIRE(glm::length(sample->weight - expected) < 1e-12);
        REQUIRE(sample->probability == Approx(bsdf.pdf(wo, sample->direction)).epsilon(1e-12));
        REQUIRE(glm::length(bsdf.evaluate(wo, sample->direction) * sample->direction.z /
                                sample->probability -
                            sample->weight) < 1e-12);
        pdf_integral += bsdf.pdf(wo, sample_hemisphere(sampler.next_2d())) * 2 * pi;
    }
    REQUIRE(pdf_integral / 5000 == Approx(1).margin(0.025));
    REQUIRE(bsdf.pdf(wo, {0, 0, -1}) == 0);
    REQUIRE(bsdf.evaluate(wo, {0, 0, -1}) == glm::dvec3(0));
    REQUIRE_FALSE(bsdf.sample({0, 0, -1}, 0.5, {0.5, 0.5}));
    REQUIRE_THROWS_AS(bsdf.pdf({0, 0, 2}, {0, 0, 1}), std::invalid_argument);
}

TEST_CASE("GGX visible-normal sampling includes the analytic null-event probability", "[bsdf]") {
    constexpr std::size_t count = 60000;
    const glm::dvec3 wo{0, 0, 1};
    for (const float roughness : {0.4F, 0.7F, 1.0F}) {
        const Bsdf bsdf(material(MaterialKind::conductor, roughness));
        Sampler sampler(87);
        std::array<std::size_t, 4> bins{};
        std::size_t nulls = 0;
        glm::dvec3 albedo(0);
        for (std::size_t index = 0; index < count; ++index) {
            const auto sample = next_sample(bsdf, wo, sampler);
            if (!sample) {
                ++nulls;
                continue;
            }
            check_sample(*sample);
            ++bins[std::min(static_cast<std::size_t>(sample->direction.z * 4), std::size_t(3))];
            albedo += sample->weight;
        }
        const double alpha = static_cast<double>(roughness) * roughness;
        const double a2 = alpha * alpha;
        const auto u = [a2](double z) { return (1 - z) / (1 + a2 + (a2 - 1) * z); };
        check_frequency(nulls, 1 - u(0), count);
        for (std::size_t bin = 0; bin < bins.size(); ++bin) {
            check_frequency(bins[bin],
                            u(static_cast<double>(bin) / 4) - u(static_cast<double>(bin + 1) / 4),
                            count);
        }
        REQUIRE(glm::all(glm::lessThanEqual(albedo / static_cast<double>(count), glm::dvec3(1))));
        if (roughness == 1) {
            REQUIRE(albedo.x / count == Approx(1 - std::log(2.0)).margin(0.005));
            // alpha=1、F0=1、wo=N 的独立解析半球反照率为 1-ln(2)。
            double quadrature = 0;
            for (int row = 0; row < 512; ++row) {
                const double z = (static_cast<double>(row) + 0.5) / 512;
                quadrature += bsdf.evaluate(wo, {std::sqrt(1 - z * z), 0, z}).x * z * 2 * pi / 512;
            }
            REQUIRE(quadrature == Approx(1 - std::log(2.0)).margin(1e-6));
        }
    }
}

TEST_CASE("GGX samples, continuous PDF and BRDF agree from smooth to grazing cases", "[bsdf]") {
    for (const float roughness : {0.0011F, 0.005F, 0.05F, 0.3F, 1.0F}) {
        auto surface = material(MaterialKind::conductor, roughness);
        surface.base_color = {0.9F, 0.6F, 0.2F};
        const Bsdf bsdf(surface);
        REQUIRE_FALSE(bsdf.is_delta());
        for (const auto wo :
             {glm::normalize(glm::dvec3(0.3, 0.4, 1)), glm::normalize(glm::dvec3(1, 0, 0.001))}) {
            Sampler sampler(100);
            for (int index = 0; index < 1000; ++index) {
                const auto sample = next_sample(bsdf, wo, sampler);
                if (!sample) {
                    continue;
                }
                check_sample(*sample);
                const auto value = bsdf.evaluate(wo, sample->direction);
                const auto reverse = bsdf.evaluate(sample->direction, wo);
                REQUIRE(glm::length(value - reverse) <= 1e-10 * (1 + glm::length(value)));
                REQUIRE(sample->probability ==
                        Approx(bsdf.pdf(wo, sample->direction)).epsilon(1e-9));
                const auto reconstructed = value * sample->direction.z / sample->probability;
                REQUIRE(glm::length(reconstructed - sample->weight) < 1e-9);
            }
        }
    }
}

TEST_CASE("Oblique GGX sample histograms match independent solid-angle quadrature", "[bsdf]") {
    constexpr std::size_t count = 40000;
    constexpr int rows = 256, columns = 512;
    for (const float roughness : {0.6F, 1.0F}) {
        const Bsdf bsdf(material(MaterialKind::conductor, roughness));
        const auto wo = glm::normalize(glm::dvec3(0.9, 0.2, 0.15));
        std::array<double, 8> expected{};
        // 在均匀立体角网格上积分公开 PDF，独立于 VNDF 的采样映射。
        for (int row = 0; row < rows; ++row) {
            const double z = (static_cast<double>(row) + 0.5) / rows;
            const double radius = std::sqrt(1 - z * z);
            for (int column = 0; column < columns; ++column) {
                const double phi = 2 * pi * (static_cast<double>(column) + 0.5) / columns;
                const glm::dvec3 wi{radius * std::cos(phi), radius * std::sin(phi), z};
                const auto bin =
                    static_cast<std::size_t>((row >= rows / 2 ? 4 : 0) + column / (columns / 4));
                expected[bin] += bsdf.pdf(wo, wi) * 2 * pi / (rows * columns);
            }
        }
        double valid_mass = 0;
        for (const auto mass : expected) {
            valid_mass += mass;
        }
        REQUIRE(valid_mass <= 1.001);
        std::array<std::size_t, 8> observed{};
        std::size_t nulls = 0;
        Sampler sampler(142);
        for (std::size_t index = 0; index < count; ++index) {
            const auto sample = next_sample(bsdf, wo, sampler);
            if (!sample) {
                ++nulls;
                continue;
            }
            double phi = std::atan2(sample->direction.y, sample->direction.x);
            if (phi < 0) {
                phi += 2 * pi;
            }
            const auto azimuth = std::min(static_cast<std::size_t>(phi * 2 / pi), std::size_t(3));
            ++observed[(sample->direction.z >= 0.5 ? 4 : 0) + azimuth];
        }
        const auto compare = [](std::size_t observed_count, double mass) {
            const double predicted = mass * count;
            const double allowance = 5 * std::sqrt(predicted * (1 - mass)) + 0.001 * count;
            REQUIRE(std::abs(static_cast<double>(observed_count) - predicted) < allowance);
        };
        compare(nulls, 1 - valid_mass);
        for (std::size_t bin = 0; bin < expected.size(); ++bin) {
            compare(observed[bin], expected[bin]);
        }
    }
}

TEST_CASE("Mirror and smooth conductor events retain discrete rather than angular mass", "[bsdf]") {
    const auto wo = glm::normalize(glm::dvec3(0.6, 0, 0.8));
    for (const auto kind : {MaterialKind::mirror, MaterialKind::conductor}) {
        auto surface = material(kind, 0);
        surface.base_color = {0.9F, 0.7F, 0.3F};
        const Bsdf bsdf(surface);
        REQUIRE(bsdf.is_delta());
        const auto sample = bsdf.sample(wo, 0.2, {0.3, 0.4});
        REQUIRE(sample);
        REQUIRE(sample->measure == SampleMeasure::discrete);
        REQUIRE(sample->probability == 1);
        REQUIRE(sample->event == ScatteringEvent::specular_reflection);
        REQUIRE(glm::length(sample->direction - glm::dvec3(-wo.x, -wo.y, wo.z)) < 1e-12);
        REQUIRE(bsdf.pdf(wo, sample->direction) == 0);
        REQUIRE(bsdf.evaluate(wo, sample->direction) == glm::dvec3(0));
        check_sample(*sample);
    }
}

TEST_CASE("Dielectric Fresnel, Snell, transport mode and total internal reflection agree",
          "[bsdf]") {
    REQUIRE(fresnel_dielectric(1, 1, 1.5) == Approx(0.04));
    REQUIRE(fresnel_dielectric(0, 1, 1.5) == 1);
    REQUIRE(fresnel_dielectric(0, 1, 1) == 0);
    REQUIRE(fresnel_dielectric(0.5, 1.5, 1) == 1);
    auto surface = material(MaterialKind::dielectric);
    surface.ior = 1.5F;
    const Bsdf entering(surface, true), leaving(surface, false);
    const auto wo = glm::normalize(glm::dvec3(0.6, 0, 0.8));
    const auto reflected = entering.sample(wo, 0, {0.5, 0.5});
    const auto transmitted = entering.sample(wo, 0.99, {0.5, 0.5});
    const auto importance = entering.sample(wo, 0.99, {0.5, 0.5}, TransportMode::importance);
    REQUIRE(reflected);
    REQUIRE(transmitted);
    REQUIRE(importance);
    REQUIRE(reflected->event == ScatteringEvent::specular_reflection);
    REQUIRE(transmitted->event == ScatteringEvent::specular_transmission);
    REQUIRE(transmitted->measure == SampleMeasure::discrete);
    REQUIRE(transmitted->direction.x == Approx(-wo.x / 1.5));
    REQUIRE(transmitted->direction.z < 0);
    REQUIRE(transmitted->eta == Approx(1.5));
    REQUIRE(transmitted->weight.x == Approx(1 / 2.25));
    REQUIRE(importance->weight.x == 1);
    REQUIRE(reflected->probability + transmitted->probability == Approx(1));
    const auto exit = leaving.sample(-transmitted->direction, 0.99, {0.5, 0.5});
    REQUIRE(exit);
    REQUIRE(transmitted->weight.x * exit->weight.x == Approx(1));
    const auto tir = leaving.sample(glm::normalize(glm::dvec3(0.9, 0, 0.3)), 0.999, {0.2, 0.5});
    REQUIRE(tir);
    REQUIRE(tir->event == ScatteringEvent::specular_reflection);
    REQUIRE(tir->probability == 1);
    REQUIRE(tir->weight == glm::dvec3(1));
    Sampler sampler(945);
    std::size_t reflections = 0;
    double average = 0;
    for (std::size_t index = 0; index < 50000; ++index) {
        const auto sample = next_sample(entering, {0, 0, 1}, sampler);
        REQUIRE(sample);
        check_sample(*sample);
        reflections += sample->event == ScatteringEvent::specular_reflection;
        average += sample->weight.x;
    }
    check_frequency(reflections, 0.04, 50000);
    REQUIRE(average / 50000 == Approx(0.04 + 0.96 / 2.25).margin(0.004));
    surface.ior = 1;
    const auto matched = Bsdf(surface).sample(wo, 0, {0, 0});
    REQUIRE(matched);
    REQUIRE(matched->probability == 1);
    REQUIRE(glm::length(matched->direction + wo) < 1e-12);
    REQUIRE_THROWS_AS(fresnel_dielectric(0.5, -1, 1), std::invalid_argument);
}
