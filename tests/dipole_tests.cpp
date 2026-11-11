#include <minirender/core/sampling.hpp>
#include <minirender/render/bsdf.hpp>
#include <minirender/render/dipole.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <numbers>

using namespace minirender;

TEST_CASE("Dipole parameters presets and material domain are explicit", "[sss]") {
    const auto marble = make_subsurface_preset("marble");
    REQUIRE(marble.parameters->reduced_scattering == glm::dvec3(2190, 2620, 3000));
    REQUIRE(marble.ior == 1.5);
    REQUIRE_NOTHROW(make_subsurface_preset("wax").parameters->validate());
    REQUIRE_THROWS(make_subsurface_preset("missing"));
    SubsurfaceParameters parameters;
    parameters.density = 0;
    REQUIRE_THROWS(parameters.validate());
    parameters = {};
    parameters.absorption.x = -1;
    REQUIRE_THROWS(parameters.validate());
    parameters = {};
    parameters.absorption.x = 0;
    parameters.reduced_scattering.x = 0;
    REQUIRE_THROWS(parameters.validate());
    Material material;
    material.kind = MaterialKind::subsurface;
    REQUIRE_THROWS(material.validate());
    material.subsurface = marble.parameters;
    REQUIRE_NOTHROW(material.validate());
    REQUIRE_THROWS(Bsdf(material));
    REQUIRE(parse_material_kind(material_kind_name(material.kind)) == material.kind);
    material.base_color_texture = "texture";
    REQUIRE_THROWS(material.validate());
}

// 独立密集余弦积分对照预计算矩，内部方向经过临界角变量变换后单独积分。
TEST_CASE("Dipole Fresnel boundary moments match independent hemispherical quadrature", "[sss]") {
    constexpr int count = 100000;
    for (const double eta : {1., 1.1, 1.5, 2., 3.}) {
        const DipoleProfile profile({}, eta);
        double external = 0, internal = 0;
        const double critical_squared = 1 - 1 / (eta * eta);
        for (int i = 0; i < count; ++i) {
            const double u = (i + .5) / count;
            external += 2 * u * (1 - fresnel_dielectric(u, 1, eta)) / count;
            const double cosine = std::sqrt(critical_squared + (1 - critical_squared) * u * u);
            internal += 2 * (1 - critical_squared) * u * fresnel_dielectric(cosine, eta, 1) / count;
        }
        internal += critical_squared;
        REQUIRE(std::abs(profile.external_transmission() - external) < 1e-8);
        REQUIRE(std::abs(profile.internal_reflectance() - internal) < 1e-8);
    }
}

TEST_CASE("Dipole profile is the normal derivative of the diffusion fluence", "[sss]") {
    SubsurfaceParameters parameters;
    parameters.absorption = {.1, .3, 0};
    parameters.reduced_scattering = {2, 3, 4};
    const DipoleProfile profile(parameters, 1.5);
    const double boundary =
        (1 + profile.internal_reflectance()) / (1 - profile.internal_reflectance());
    for (const double r : {0., .01, .3, 2.}) {
        for (int c = 0; c < 3; ++c) {
            const double total = parameters.absorption[c] + parameters.reduced_scattering[c];
            const double diffusion = 1 / (3 * total);
            const double zr = 1 / total, zv = zr + 4 * boundary * diffusion;
            const double extinction = std::sqrt(3 * parameters.absorption[c] * total);
            const double albedo = parameters.reduced_scattering[c] / total;
            const auto fluence = [&](double z) {
                const double dr = std::hypot(r, z - zr), dv = std::hypot(r, z + zv);
                return albedo / (4 * std::numbers::pi * diffusion) *
                       (std::exp(-extinction * dr) / dr - std::exp(-extinction * dv) / dv);
            };
            const double h = zr * 1e-5;
            const double derivative = diffusion * (fluence(h) - fluence(-h)) / (2 * h);
            REQUIRE(std::abs(derivative / profile.evaluate(r)[c] - 1) < 1e-8);
        }
    }
}

// r=z*t/(1-t) 将整个正半轴映到有限区间，积分独立于 cumulative 和采样实现。
TEST_CASE("Dipole area integral and radial inverse retain energy including zero absorption",
          "[sss]") {
    for (const double absorption : {0., .01, 1.}) {
        const SubsurfaceParameters parameters{{absorption, absorption, absorption}, {1, 2, 3}, 1};
        const DipoleProfile profile(parameters, 1.5);
        glm::dvec3 integral(0);
        constexpr int count = 100000;
        for (int i = 0; i < count; ++i) {
            const double t = (i + .5) / count;
            const double r = t / (1 - t);
            integral +=
                2 * std::numbers::pi * r * profile.evaluate(r) / ((1 - t) * (1 - t) * count);
        }
        REQUIRE(glm::length(integral - profile.reflectance()) < 1e-8);
        for (int c = 0; c < 3; ++c) {
            const double bound = 20;
            for (const double u : {0., 1e-9, .01, .1, .5, .9, .999999}) {
                const double radius = profile.sample_radius(c, u, bound);
                REQUIRE(radius >= 0);
                REQUIRE(radius < bound);
                REQUIRE(std::abs(profile.cumulative(radius)[c] / profile.cumulative(bound)[c] - u) <
                        1e-12);
            }
        }
        REQUIRE_THROWS(profile.sample_radius(0, 1, 2));
        REQUIRE_THROWS(profile.sample_radius(3, .5, 2));
        REQUIRE_THROWS(profile.evaluate(-1));
    }
}

TEST_CASE("Dipole density scales distances and area density consistently", "[sss]") {
    SubsurfaceParameters parameters;
    const DipoleProfile baseline(parameters, 1.5);
    parameters.density = 7;
    const DipoleProfile dense(parameters, 1.5);
    REQUIRE(glm::length(baseline.reflectance() - dense.reflectance()) < 1e-15);
    for (const double radius : {0., .0001, .001, .01}) {
        const auto a = baseline.evaluate(radius), b = dense.evaluate(radius / 7) / 49.;
        REQUIRE(glm::length(a - b) / glm::length(a) < 1e-13);
        REQUIRE(glm::length(baseline.cumulative(radius) - dense.cumulative(radius / 7)) < 1e-14);
    }
}

TEST_CASE("Dipole radius histograms match independent partial area integrals", "[sss]") {
    const DipoleProfile profile({{0, .01, 1}, {1, 2, 3}, 1}, 1.5);
    constexpr std::array edges{0., .05, .1, .2, .5, 1., 2., 4.};
    constexpr int samples = 30000;
    for (int channel = 0; channel < 3; ++channel) {
        std::array<double, edges.size() - 1> mass{};
        std::array<int, edges.size() - 1> counts{};
        double total = 0;
        for (std::size_t bin = 0; bin < mass.size(); ++bin) {
            constexpr int steps = 6000;
            const double width = (edges[bin + 1] - edges[bin]) / steps;
            for (int i = 0; i < steps; ++i) {
                const double r = edges[bin] + (i + .5) * width;
                mass[bin] += 2 * std::numbers::pi * r * profile.evaluate(r)[channel] * width;
            }
            total += mass[bin];
        }
        Sampler random(std::uint64_t(701 + channel));
        for (int i = 0; i < samples; ++i) {
            const double r = profile.sample_radius(channel, random.next_1d(), edges.back());
            for (std::size_t bin = 0; bin < counts.size(); ++bin) {
                if (r >= edges[bin] && r < edges[bin + 1]) {
                    ++counts[bin];
                    break;
                }
            }
        }
        for (std::size_t bin = 0; bin < counts.size(); ++bin) {
            const double probability = mass[bin] / total;
            const double expected = samples * probability;
            const double sigma = std::sqrt(samples * probability * (1 - probability));
            REQUIRE(std::abs(counts[bin] - expected) < 6 * sigma + 3);
        }
    }
}
