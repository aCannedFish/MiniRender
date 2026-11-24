// 白炉积分、三种方向反照率估计及混合 BSDF 的 sample/eval/pdf 一致性。
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <minirender/render/bsdf.hpp>
#include <minirender/render/microfacet_energy.hpp>
#include <numbers>
using namespace minirender;

TEST_CASE("GGX energy integrals agree and retain the smooth limit") {
    const auto reference =
        integrate_directional_energy(.6, .8, 16384, 101, EnergyMethod::split_sum);
    for (auto method :
         {EnergyMethod::uniform, EnergyMethod::visible_normal, EnergyMethod::split_sum}) {
        REQUIRE(std::abs(integrate_directional_energy(.6, .8, 16384, 42, method) - reference) <
                .025);
        REQUIRE(integrate_directional_energy(.5, 0, 16, 101, method) == 1.);
    }
    Image linear(5, 2);
    for (std::size_t x = 0; x < 5; ++x) {
        linear.at(x, 0) = {float(x) * .25F, 0, 0};
    }
    store_average_energy(linear, 0);
    REQUIRE(std::abs(linear.at(1, 0).b - 2. / 3.) < 1e-6);
}

TEST_CASE("Kulla Conty white furnace recovers the lost microfacet energy") {
    Material metal{"white", "White", MaterialKind::conductor, glm::vec3(1)};
    metal.roughness = 1;
    const Bsdf single(metal);
    metal.compensate_energy = true;
    const Bsdf multiple(metal);
    for (double mu : {.2, .6, 1.}) {
        const glm::dvec3 wo{std::sqrt(1. - mu * mu), 0, mu};
        double           single_energy = 0, multi_energy = 0;
        for (int y = 0; y < 64; ++y) {
            const double z = (y + .5) / 64.;
            for (int x = 0; x < 96; ++x) {
                const double     phi = (x + .5) * 2. * std::numbers::pi / 96.;
                const glm::dvec3 wi{std::sqrt(1 - z * z) * std::cos(phi),
                                    std::sqrt(1 - z * z) * std::sin(phi), z};
                const double     weight = 2. * std::numbers::pi * z / (64. * 96.);
                single_energy += single.evaluate(wo, wi).r * weight;
                multi_energy += multiple.evaluate(wo, wi).r * weight;
            }
        }
        REQUIRE(single_energy < .85);
        REQUIRE(std::abs(multi_energy - 1.) < .015);
    }
}

TEST_CASE("Compensated conductor uses its mixed solid-angle PDF for MIS") {
    Material metal{"copper", "Copper", MaterialKind::conductor, {.95F, .64F, .54F}};
    metal.roughness         = .85F;
    metal.compensate_energy = true;
    Bsdf       bsdf(metal);
    Sampler    sampler(53);
    glm::dvec3 wo = glm::normalize(glm::dvec3(.4, 0, .8));
    glm::dvec3 mean(0), reference(0);
    for (int i = 0; i < 16384; ++i) {
        const auto component = sampler.next_1d();
        if (auto sample = bsdf.sample(wo, component, sampler.next_2d())) {
            auto density = bsdf.pdf(wo, sample->direction);
            REQUIRE(std::abs(sample->probability - density) < 1e-8);
            const auto eval = bsdf.evaluate(wo, sample->direction);
            REQUIRE(glm::length(sample->weight - eval * sample->direction.z / density) < 1e-8);
            mean += sample->weight;
        }
        const auto wi = sample_cosine_hemisphere(sampler.next_2d());
        reference += std::numbers::pi * bsdf.evaluate(wo, wi);
    }
    REQUIRE(glm::length((mean - reference) / 16384.) < .025);
    REQUIRE(mean.r > mean.g);
    REQUIRE(mean.g > mean.b);
}
