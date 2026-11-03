#include <minirender/render/bsdf.hpp>
#include <minirender/render/ibl.hpp>
#include <minirender/render/light.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>

using namespace minirender;

namespace {

constexpr double pi = std::numbers::pi;
IblEnvironmentSettings small_settings() {
    IblEnvironmentSettings settings;
    settings.diffuse_width = 8;
    settings.specular_width = 16;
    settings.specular_layers = 3;
    settings.diffuse_samples = 128;
    settings.specular_samples = 128;
    return settings;
}
glm::dvec3 direction(std::size_t x, std::size_t y, std::size_t width, std::size_t height) {
    const auto phi = 2 * pi * (static_cast<double>(x) + 0.5) / static_cast<double>(width);
    const auto theta = pi * (static_cast<double>(y) + 0.5) / static_cast<double>(height);
    return {std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)};
}
std::shared_ptr<const Texture> constant(glm::vec3 color, std::size_t width = 8,
                                        std::size_t height = 4) {
    Image image(width, height);
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            image.at(x, y) = color;
        }
    }
    return std::make_shared<const Texture>(std::move(image));
}
void constant_image(const Image& image, glm::vec3 value, float tolerance = 1e-5F) {
    for (std::size_t y = 0; y < image.height(); ++y) {
        for (std::size_t x = 0; x < image.width(); ++x) {
            REQUIRE(glm::length(image.at(x, y) - value) <= tolerance);
        }
    }
}
double sphere_integral(const Image& image) {
    double sum = 0;
    // 对包含极点的 theta 节点，精确积分分段线性插值乘以 sin(theta)。
    const double step = pi / static_cast<double>(image.height() - 1);
    for (std::size_t y = 0; y + 1 < image.height(); ++y) {
        const double a = step * static_cast<double>(y), b = a + step;
        const double difference = (std::sin(b) - std::sin(a)) / step;
        const double first = std::cos(a) - difference;
        const double second = difference - std::cos(b);
        for (std::size_t x = 0; x < image.width(); ++x) {
            sum += image.at(x, y).x * first + image.at(x, y + 1).x * second;
        }
    }
    return sum * 2 * pi / static_cast<double>(image.width());
}

} // namespace

TEST_CASE("IBL latlong agrees with path lighting across axes seams poles and rotations", "[ibl]") {
    Image image(8, 4);
    for (std::size_t y = 0; y < 4; ++y) {
        for (std::size_t x = 0; x < 8; ++x) {
            image.at(x, y) = {static_cast<float>(x), static_cast<float>(y),
                              static_cast<float>(x + y)};
        }
    }
    SceneSnapshot scene{};
    scene.environment_texture = std::make_shared<const Texture>(image);
    Sampler random(409);
    for (const float rotation : {-360.0F, -90.0F, 0.0F, 90.0F, 370.0F}) {
        scene.environment_rotation_degrees = rotation;
        const EnvironmentLight reference(scene);
        for (const auto d : {glm::dvec3(1, 0, 0), glm::dvec3(-1, 0, 0), glm::dvec3(0, 1, 0),
                             glm::dvec3(0, -1, 0), glm::dvec3(0, 0, 1), glm::dvec3(0, 0, -1)}) {
            REQUIRE(glm::length(sample_ibl_latlong(image, d, rotation) - reference.evaluate(d)) <
                    1e-12);
        }
        for (int sample = 0; sample < 100; ++sample) {
            const auto d = sample_sphere(random.next_2d());
            REQUIRE(glm::length(sample_ibl_latlong(image, d, rotation) - reference.evaluate(d)) <
                    1e-12);
        }
    }
    const auto seam_a = sample_ibl_latlong(image, glm::normalize(glm::dvec3(1, 0, 1e-10)));
    const auto seam_b = sample_ibl_latlong(image, glm::normalize(glm::dvec3(1, 0, -1e-10)));
    REQUIRE(glm::length(seam_a - seam_b) < 1e-8);
    REQUIRE_THROWS_AS(sample_ibl_latlong(image, {0, 0, 0}), std::invalid_argument);
    REQUIRE_THROWS_AS(sample_ibl_latlong(image, {1, 0, 0}, std::numeric_limits<double>::infinity()),
                      std::invalid_argument);
}

TEST_CASE("IBL constant environments retain linear HDR in every roughness layer", "[ibl]") {
    const glm::vec3 value{2, 0.5F, 7};
    const auto source = constant(value);
    std::vector<IblProgress> progress;
    const auto settings = small_settings();
    const auto maps = bake_ibl_environment(source, settings, {},
                                           [&](const IblProgress& p) { progress.push_back(p); });
    REQUIRE(maps->source == source);
    constant_image(maps->diffuse, value);
    REQUIRE(maps->specular.size() == settings.specular_layers);
    for (const auto& layer : maps->specular) {
        REQUIRE(layer.width() == settings.specular_width);
        REQUIRE(layer.height() == settings.specular_width / 2);
        constant_image(layer, value);
    }
    REQUIRE(progress.front().stage == IblStage::source);
    REQUIRE(progress.back().stage == IblStage::complete);
    for (std::size_t i = 0; i < progress.size(); ++i) {
        REQUIRE(progress[i].completed <= progress[i].total);
        if (i && progress[i].stage == progress[i - 1].stage) {
            REQUIRE(progress[i].completed >= progress[i - 1].completed);
            REQUIRE(progress[i].total == progress[i - 1].total);
        }
    }
    const auto frozen = maps->specular[1].at(2, 1);
    const auto black = bake_ibl_environment(constant(glm::vec3(0)), settings);
    constant_image(black->diffuse, glm::vec3(0));
    REQUIRE(maps->specular[1].at(2, 1) == frozen);
}

TEST_CASE("IBL diffuse and fully rough maps preserve directional first moments", "[ibl]") {
    Image image(128, 64);
    for (std::size_t y = 0; y < image.height(); ++y) {
        for (std::size_t x = 0; x < image.width(); ++x) {
            const auto d = direction(x, y, image.width(), image.height());
            image.at(x, y) = glm::vec3(1.0 + 0.5 * d);
        }
    }
    const auto source = std::make_shared<const Texture>(std::move(image));
    auto settings = small_settings();
    settings.diffuse_width = 16;
    settings.diffuse_samples = 1024;
    settings.specular_samples = 1024;
    const auto maps = bake_ibl_environment(source, settings);
    double diffuse_error = 0, specular_error = 0;
    for (const auto normal : {glm::dvec3(1, 0, 0), glm::dvec3(-1, 0, 0), glm::dvec3(0, 1, 0),
                              glm::dvec3(0, -1, 0), glm::dvec3(0, 0, 1), glm::dvec3(0, 0, -1)}) {
        // ∫ L cos/π 对 L=1+0.5*d 的解析结果为 1+n/3。
        const auto expected = 1.0 + normal / 3.0;
        diffuse_error =
            std::max(diffuse_error, glm::length(sample_ibl_diffuse(*maps, normal) - expected));
        specular_error =
            std::max(specular_error, glm::length(sample_ibl_specular(*maps, normal, 1) - expected));
        REQUIRE(glm::length(sample_ibl_diffuse(*maps, normal) - expected) < 0.045);
        // alpha=1、N=V 的 GGX 预滤波退化为余弦加权环境平均。
        REQUIRE(glm::length(sample_ibl_specular(*maps, normal, 1) - expected) < 0.05);
        REQUIRE(glm::length(sample_ibl_specular(*maps, normal, 0) -
                            sample_ibl_latlong(source->level(0), normal)) < 0.04);
    }
    const auto rotated = sample_ibl_diffuse(*maps, {1, 0, 0}, 90);
    REQUIRE(glm::length(rotated - sample_ibl_diffuse(*maps, {0, 0, 1})) < 1e-12);
    const auto up = sample_ibl_specular(*maps, {0, 1, 0}, 1);
    const auto down = sample_ibl_specular(*maps, {0, -1, 0}, 1);
    REQUIRE(up.y - down.y > 0.6); // 高粗糙度仍保留角度变化，不能退化为 1×1。
    for (const double angle : {0.0, 45.0, 120.0, 270.0}) {
        REQUIRE(glm::length(sample_ibl_diffuse(*maps, {0, 1, 0}, angle) -
                            sample_ibl_diffuse(*maps, {0, 1, 0})) < 1e-12);
    }
    const auto repeated = bake_ibl_environment(source, settings);
    for (std::size_t y = 0; y < maps->diffuse.height(); ++y) {
        for (std::size_t x = 0; x < maps->diffuse.width(); ++x) {
            REQUIRE(maps->diffuse.at(x, y) == repeated->diffuse.at(x, y));
        }
    }
    for (std::size_t layer = 0; layer < maps->specular.size(); ++layer) {
        for (std::size_t y = 0; y < maps->specular[layer].height(); ++y) {
            for (std::size_t x = 0; x < maps->specular[layer].width(); ++x) {
                REQUIRE(maps->specular[layer].at(x, y) == repeated->specular[layer].at(x, y));
            }
        }
    }
    std::cout << "IBL first moment max diffuse/specular errors: " << diffuse_error << ' '
              << specular_error << '\n';
}

TEST_CASE("IBL roughness spreads a small bright source while retaining spherical energy", "[ibl]") {
    Image image(64, 32);
    image.at(0, 16) = glm::vec3(20);
    auto settings = small_settings();
    settings.specular_width = 64;
    settings.specular_samples = 512;
    const auto maps =
        bake_ibl_environment(std::make_shared<const Texture>(std::move(image)), settings);
    const auto& sharp = maps->specular.front();
    const auto& rough = maps->specular.back();
    float sharp_peak = 0, rough_peak = 0;
    std::size_t sharp_lit = 0, rough_lit = 0;
    for (std::size_t y = 0; y < sharp.height(); ++y) {
        for (std::size_t x = 0; x < sharp.width(); ++x) {
            sharp_peak = std::max(sharp_peak, sharp.at(x, y).x);
            rough_peak = std::max(rough_peak, rough.at(x, y).x);
            sharp_lit += sharp.at(x, y).x > 1e-5F;
            rough_lit += rough.at(x, y).x > 1e-5F;
        }
    }
    INFO("peaks " << sharp_peak << " / " << rough_peak << " energy " << sphere_integral(sharp)
                  << " / " << sphere_integral(rough));
    REQUIRE(sharp_peak > 19);
    REQUIRE(rough_peak < sharp_peak * 0.1F);
    REQUIRE(rough_lit > sharp_lit * 10);
    std::cout << "IBL bright source peaks: " << sharp_peak << ' ' << rough_peak
              << "; energy: " << sphere_integral(sharp) << ' ' << sphere_integral(rough) << '\n';
    REQUIRE(std::abs(sphere_integral(rough) / sphere_integral(sharp) - 1) < 0.25);
}

TEST_CASE("IBL BRDF coefficients match mirror limits energy and independent quadrature", "[ibl]") {
    for (const auto cosine : {0.0, 0.2, 0.5, 1.0}) {
        const auto ab = integrate_ibl_brdf(cosine, 0, 16);
        REQUIRE(std::abs(ab.y - std::pow(1 - cosine, 5)) < 1e-12);
        REQUIRE(std::abs(ab.x + ab.y - 1) < 1e-12);
    }
    const auto rough_normal = integrate_ibl_brdf(1, 1, 16384);
    std::cout << "IBL white rough normal energy: " << rough_normal.x + rough_normal.y
              << "; reference: " << 1 - std::log(2.0) << '\n';
    REQUIRE(std::abs(rough_normal.x + rough_normal.y - (1 - std::log(2.0))) < 0.002);
    for (const auto cosine : {0.0, 1e-4, 0.2, 0.7, 1.0}) {
        for (const auto roughness : {0.05, 0.5, 1.0}) {
            const auto ab = integrate_ibl_brdf(cosine, roughness, 2048);
            REQUIRE(ab.x >= 0);
            REQUIRE(ab.y >= 0);
            REQUIRE(ab.x + ab.y <= 1.000001);
        }
    }
    for (const double cosine : {0.25, 0.7, 1.0}) {
        for (const float roughness : {0.5F, 1.0F}) {
            Material metal;
            metal.kind = MaterialKind::conductor;
            metal.base_color = {0.15F, 0.5F, 0.9F};
            metal.roughness = roughness;
            const Bsdf reference(metal);
            const glm::dvec3 wo{std::sqrt(1 - cosine * cosine), 0, cosine};
            glm::dvec3 integral(0);
            constexpr int side = 256;
            for (int y = 0; y < side; ++y) {
                const double z = (static_cast<double>(y) + 0.5) / side;
                for (int x = 0; x < side; ++x) {
                    const double phi = 2 * pi * (static_cast<double>(x) + 0.5) / side;
                    const double sine = std::sqrt(1 - z * z);
                    integral +=
                        reference.evaluate(wo, {sine * std::cos(phi), sine * std::sin(phi), z}) * z;
                }
            }
            integral *= 2 * pi / (side * side);
            const auto ab = integrate_ibl_brdf(cosine, roughness, 16384, 302);
            const auto actual = glm::dvec3(metal.base_color) * ab.x + ab.y;
            INFO("NdotV " << cosine << " roughness " << roughness << " error "
                          << glm::length(actual - integral));
            REQUIRE(glm::length(actual - integral) < 0.006);
            std::cout << "IBL BRDF quadrature NdotV/roughness/error: " << cosine << ' ' << roughness
                      << ' ' << glm::length(actual - integral) << '\n';
        }
    }
}

TEST_CASE("IBL LUT parameter endpoints and interpolation retain the BRDF model", "[ibl]") {
    const auto lut = bake_ibl_brdf({16, 1024, 101});
    REQUIRE(sample_ibl_brdf(*lut, 0, 0) == glm::dvec2(0, 1));
    REQUIRE(sample_ibl_brdf(*lut, 1, 0) == glm::dvec2(1, 0));
    const auto value = sample_ibl_brdf(*lut, 0.63, 0.37);
    REQUIRE(glm::length(value - integrate_ibl_brdf(0.63, 0.37, 16384, 53)) < 0.025);
    REQUIRE_THROWS_AS(sample_ibl_brdf(*lut, -0.1, 0.5), std::invalid_argument);
    REQUIRE_THROWS_AS(integrate_ibl_brdf(0.5, 1.1, 128), std::invalid_argument);
}

TEST_CASE("IBL source clamping precedes mip reduction and preserves the original asset", "[ibl]") {
    Image image(64, 32);
    for (std::size_t y = 0; y < image.height(); ++y) {
        for (std::size_t x = 0; x < image.width(); ++x) {
            image.at(x, y) = glm::vec3(x % 2 ? -2 : 2);
        }
    }
    const auto source = std::make_shared<const Texture>(std::move(image));
    constant_image(source->level(1), glm::vec3(0));
    auto settings = small_settings();
    settings.source_maximum_dimension = 16;
    const auto maps = bake_ibl_environment(source, settings);
    REQUIRE(maps->source->level(0).width() == 16);
    constant_image(maps->source->level(0), glm::vec3(1));
    constant_image(maps->diffuse, glm::vec3(1));
    for (const auto& layer : maps->specular) {
        constant_image(layer, glm::vec3(1));
    }
    REQUIRE(source->level(0).at(1, 0) == glm::vec3(-2));
}

TEST_CASE("IBL preparation and sampling report progress and propagate cancellation", "[ibl]") {
    for (const auto stage :
         {IblStage::source, IblStage::mipmaps, IblStage::diffuse, IblStage::specular}) {
        CancellationSource cancellation;
        REQUIRE_THROWS_AS(
            bake_ibl_environment(constant(glm::vec3(1)), small_settings(), cancellation.token(),
                                 [&](const IblProgress& p) {
                                     if (p.stage == stage && (p.completed > 0 || p.total == 0)) {
                                         cancellation.request();
                                     }
                                 }),
            OperationCancelled);
    }
    CancellationSource cancellation;
    REQUIRE_THROWS_AS(bake_ibl_brdf({16, 128, 1}, cancellation.token(),
                                    [&](const IblProgress& p) {
                                        if (p.completed > 0) {
                                            cancellation.request();
                                        }
                                    }),
                      OperationCancelled);
    REQUIRE_THROWS_AS(Texture(Image(8, 4), cancellation.token()), OperationCancelled);
    REQUIRE_THROWS_AS(integrate_ibl_brdf(0.5, 0.5, 1024, 1, cancellation.token()),
                      OperationCancelled);
    REQUIRE_NOTHROW(bake_ibl_brdf({4, 16, 1}));
}

TEST_CASE("IBL settings bound dimensions layers and computational work", "[ibl]") {
    auto settings = small_settings();
    REQUIRE_NOTHROW(settings.validate());
    settings.specular_width = 15;
    REQUIRE_THROWS_AS(settings.validate(), std::invalid_argument);
    settings = small_settings();
    settings.specular_samples = 1;
    REQUIRE_THROWS_AS(settings.validate(), std::invalid_argument);
    settings.specular_samples = 4096;
    settings.specular_layers = 16;
    settings.specular_width = 512;
    REQUIRE_THROWS_AS(settings.validate(), std::invalid_argument);
    REQUIRE_THROWS_AS(bake_ibl_environment(nullptr), std::invalid_argument);
    REQUIRE_THROWS_AS(bake_ibl_brdf({256, 16384, 1}), std::invalid_argument);
}
