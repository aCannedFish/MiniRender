#include <minirender/render/bsdf.hpp>
#include <minirender/render/ibl.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace minirender {
namespace {

constexpr double pi = std::numbers::pi;

bool power_of_two(std::size_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}
void parameter(double value, const char* name) {
    if (!std::isfinite(value) || value < 0 || value > 1) {
        throw std::invalid_argument(std::string(name) + " must be in [0,1]");
    }
}

// Hammersley 的第二维反转 32 个二进制位。逐像素的固定平移降低不同方向间的采样相关。
double radical_inverse(std::uint32_t bits) {
    bits = (bits << 16U) | (bits >> 16U);
    bits = ((bits & 0x55555555U) << 1U) | ((bits & 0xAAAAAAAAU) >> 1U);
    bits = ((bits & 0x33333333U) << 2U) | ((bits & 0xCCCCCCCCU) >> 2U);
    bits = ((bits & 0x0F0F0F0FU) << 4U) | ((bits & 0xF0F0F0F0U) >> 4U);
    bits = ((bits & 0x00FF00FFU) << 8U) | ((bits & 0xFF00FF00U) >> 8U);
    return static_cast<double>(bits) * 0x1p-32;
}
glm::dvec2 sequence(std::uint32_t index, std::uint32_t count, glm::dvec2 shift) {
    const glm::dvec2 point{(static_cast<double>(index) + 0.5) / count, radical_inverse(index)};
    const auto moved = point + shift;
    return moved - glm::floor(moved);
}
glm::dvec2 rotation(std::uint64_t seed, std::uint64_t pixel, std::uint64_t stage) {
    auto random = Sampler::for_sample(seed, pixel, stage);
    return random.next_2d();
}

void report(const IblProgressCallback& callback, IblStage stage, std::uint64_t done,
            std::uint64_t total, const CancellationToken& cancellation) {
    if (callback) {
        callback({stage, done, total});
    }
    cancellation.check();
}

glm::dvec2 coordinates(glm::dvec3 direction, double rotation_degrees) {
    const double length2 = glm::dot(direction, direction);
    if (!std::isfinite(length2) || std::abs(length2 - 1) > 1e-4 ||
        !std::isfinite(rotation_degrees)) {
        throw std::invalid_argument("IBL lookup requires a unit direction and finite rotation");
    }
    direction = glm::normalize(direction);
    const double angle = std::remainder(rotation_degrees, 360.0) * pi / 180;
    double u = (std::atan2(direction.z, direction.x) + angle) / (2 * pi);
    u -= std::floor(u);
    const double v = std::atan2(std::hypot(direction.x, direction.z), direction.y) / pi;
    return {u, v};
}
glm::dvec3 direction_at(std::size_t x, std::size_t y, const Image& image) {
    if (y == 0) {
        return {0, 1, 0};
    }
    if (y + 1 == image.height()) {
        return {0, -1, 0};
    }
    const double phi = 2 * pi * (static_cast<double>(x) + 0.5) / static_cast<double>(image.width());
    const double theta = pi * static_cast<double>(y) / static_cast<double>(image.height() - 1);
    return {std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)};
}

glm::dvec3 bilinear(const Image& image, double x, double y, bool repeat_x, bool nonnegative) {
    const auto ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
    const double tx = x - std::floor(x), ty = y - std::floor(y);
    const auto texel = [&](int column, int row) {
        const auto width = static_cast<int>(image.width()),
                   height = static_cast<int>(image.height());
        column = repeat_x ? (column % width + width) % width : std::clamp(column, 0, width - 1);
        auto color = glm::dvec3(image.at(static_cast<std::size_t>(column),
                                         static_cast<std::size_t>(std::clamp(row, 0, height - 1))));
        return nonnegative ? glm::max(color, glm::dvec3(0)) : color;
    };
    return glm::mix(glm::mix(texel(ix, iy), texel(ix + 1, iy), tx),
                    glm::mix(texel(ix, iy + 1), texel(ix + 1, iy + 1), tx), ty);
}
glm::dvec3 latlong(const Image& image, glm::dvec2 uv) {
    return bilinear(image, uv.x * static_cast<double>(image.width()) - 0.5,
                    uv.y * static_cast<double>(image.height()) - 0.5, true, true);
}
glm::dvec3 baked_latlong(const Image& image, glm::dvec2 uv) {
    return bilinear(image, uv.x * static_cast<double>(image.width()) - 0.5,
                    uv.y * static_cast<double>(image.height() - 1), true, true);
}
glm::dvec3 source_lookup(const Texture& texture, glm::dvec3 direction, double lod) {
    const auto uv = coordinates(direction, 0);
    const double level = std::clamp(lod, 0.0, static_cast<double>(texture.level_count() - 1));
    const auto lower = static_cast<std::size_t>(level);
    const auto upper = std::min(lower + 1, texture.level_count() - 1);
    return glm::mix(latlong(texture.level(lower), uv), latlong(texture.level(upper), uv),
                    level - static_cast<double>(lower));
}

// 先对原图截去负辐射，再生成 mip，避免正负像素先平均而丢掉照明。
// 正常非负图像直接复用已有 mip；最终仅保留有界的预处理源。
std::shared_ptr<const Texture> prepare_source(std::shared_ptr<const Texture> source,
                                              std::size_t maximum_dimension,
                                              const CancellationToken& cancellation,
                                              const IblProgressCallback& progress) {
    const auto& base = source->level(0);
    const auto total = static_cast<std::uint64_t>(base.width()) * base.height();
    report(progress, IblStage::source, 0, total, cancellation);
    bool negative = false;
    for (std::size_t y = 0; y < base.height(); ++y) {
        for (std::size_t x = 0; x < base.width(); ++x) {
            negative = negative || glm::any(glm::lessThan(base.at(x, y), glm::vec3(0)));
        }
        report(progress, IblStage::source, (y + 1) * base.width(), total, cancellation);
    }
    report(progress, IblStage::mipmaps, 0, 0, cancellation);
    if (negative) {
        Image positive(base.width(), base.height());
        for (std::size_t y = 0; y < base.height(); ++y) {
            cancellation.check();
            for (std::size_t x = 0; x < base.width(); ++x) {
                positive.at(x, y) = glm::max(base.at(x, y), glm::vec3(0));
            }
        }
        source = std::make_shared<const Texture>(std::move(positive), cancellation);
    }
    std::size_t level = 0;
    while (std::max(source->level(level).width(), source->level(level).height()) >
           maximum_dimension) {
        ++level;
    }
    if (level != 0) {
        const auto& selected = source->level(level);
        Image bounded(selected.width(), selected.height());
        for (std::size_t y = 0; y < selected.height(); ++y) {
            cancellation.check();
            for (std::size_t x = 0; x < selected.width(); ++x) {
                bounded.at(x, y) = selected.at(x, y);
            }
        }
        source = std::make_shared<const Texture>(std::move(bounded), cancellation);
    }
    return source;
}

// FIS 的纹素立体角按 latlong 行精确计算，极点附近不使用固定的平均面积。
double source_lod(const Texture& source, glm::dvec3 direction, double pdf, std::uint32_t samples) {
    const auto& image = source.level(0);
    const auto uv = coordinates(direction, 0);
    const auto row = std::min(static_cast<std::size_t>(uv.y * static_cast<double>(image.height())),
                              image.height() - 1);
    const double theta0 = pi * static_cast<double>(row) / static_cast<double>(image.height());
    const double theta1 = pi * static_cast<double>(row + 1) / static_cast<double>(image.height());
    const double solid_angle = 2 * pi / static_cast<double>(image.width()) * 2 *
                               std::sin((theta0 + theta1) * 0.5) *
                               std::sin((theta1 - theta0) * 0.5);
    return 0.5 * std::log2(1 / (static_cast<double>(samples) * pdf * solid_angle));
}

} // namespace

void IblEnvironmentSettings::validate() const {
    const bool dimensions = power_of_two(source_maximum_dimension) &&
                            source_maximum_dimension >= 16 && source_maximum_dimension <= 4096 &&
                            power_of_two(diffuse_width) && diffuse_width >= 4 &&
                            diffuse_width <= 256 && power_of_two(specular_width) &&
                            specular_width >= 8 && specular_width <= 512;
    if (!dimensions || specular_layers < 2 || specular_layers > 16 || diffuse_samples == 0 ||
        diffuse_samples > 4096 || specular_samples < 2 || specular_samples > 4096) {
        throw std::invalid_argument(
            "IBL dimensions, layer count or samples exceed supported ranges");
    }
    const std::uint64_t work =
        diffuse_width * diffuse_width / 2 * diffuse_samples +
        specular_width * specular_width / 2 * (specular_layers - 1) * specular_samples;
    if (work > 512ULL * 1024 * 1024) {
        throw std::invalid_argument("IBL environment exceeds the sample budget");
    }
}
void IblBrdfSettings::validate() const {
    if (!power_of_two(resolution) || resolution < 4 || resolution > 256 || samples == 0 ||
        samples > 16384 || resolution * resolution * samples > 128ULL * 1024 * 1024) {
        throw std::invalid_argument(
            "IBL BRDF requires a 4..256 power-of-two grid within the sample budget");
    }
}

glm::dvec3 sample_ibl_latlong(const Image& image, glm::dvec3 direction, double rotation_degrees) {
    return latlong(image, coordinates(direction, rotation_degrees));
}
glm::dvec3 sample_ibl_diffuse(const IblEnvironment& maps, glm::dvec3 normal,
                              double rotation_degrees) {
    return baked_latlong(maps.diffuse, coordinates(normal, rotation_degrees));
}
glm::dvec3 sample_ibl_specular(const IblEnvironment& maps, glm::dvec3 reflection, double roughness,
                               double rotation_degrees) {
    parameter(roughness, "Roughness");
    if (maps.specular.size() < 2) {
        throw std::invalid_argument("IBL specular maps require at least two roughness layers");
    }
    const double layer = roughness * static_cast<double>(maps.specular.size() - 1);
    const auto lower = static_cast<std::size_t>(layer);
    const auto upper = std::min(lower + 1, maps.specular.size() - 1);
    const auto uv = coordinates(reflection, rotation_degrees);
    return glm::mix(baked_latlong(maps.specular[lower], uv),
                    baked_latlong(maps.specular[upper], uv), layer - static_cast<double>(lower));
}
glm::dvec2 sample_ibl_brdf(const Image& lut, double normal_dot_view, double roughness) {
    parameter(normal_dot_view, "NdotV");
    parameter(roughness, "Roughness");
    if (lut.width() < 2 || lut.height() < 2) {
        throw std::invalid_argument("IBL BRDF grid must contain its parameter endpoints");
    }
    return glm::dvec2(bilinear(lut, normal_dot_view * static_cast<double>(lut.width() - 1),
                               roughness * static_cast<double>(lut.height() - 1), false, false));
}

glm::dvec2 integrate_ibl_brdf(double normal_dot_view, double roughness, std::uint32_t samples,
                              std::uint64_t seed, const CancellationToken& cancellation) {
    cancellation.check();
    parameter(normal_dot_view, "NdotV");
    parameter(roughness, "Roughness");
    if (samples == 0 || samples > 65536) {
        throw std::invalid_argument("IBL BRDF integration requires 1..65536 samples");
    }
    if (roughness * roughness <= 1e-6) {
        const double fresnel = std::pow(1 - normal_dot_view, 5);
        return {1 - fresnel, fresnel};
    }
    // 节点 NdotV=0 使用连续模型的掠射极限近似，避免向 BSDF 传入零余弦。
    const double cosine = std::max(normal_dot_view, 1e-6);
    const glm::dvec3 wo{std::sqrt(std::max(0.0, 1 - cosine * cosine)), 0, cosine};
    Material white;
    white.kind = MaterialKind::conductor;
    white.base_color = glm::vec3(1);
    white.roughness = static_cast<float>(roughness);
    const Bsdf bsdf(white);
    const auto shift = rotation(seed, 0, 0);
    glm::dvec2 sum(0);
    for (std::uint32_t i = 0; i < samples; ++i) {
        if ((i & 63U) == 0) {
            cancellation.check();
        }
        const auto sample = bsdf.sample(wo, 0.5, sequence(i, samples, shift));
        if (!sample) {
            continue;
        }
        const auto halfway = glm::normalize(wo + sample->direction);
        // 白色 F0 使 Fresnel 恒为 1，weight 是不含 Fresnel 的 f*cos/pdf。
        // 同一 VNDF 分布下乘 (1-Fc) 和 Fc，分别得到 F0 的系数 A 与常数项 B。
        const double fc = std::pow(1 - std::clamp(glm::dot(wo, halfway), 0.0, 1.0), 5);
        sum += sample->weight.x * glm::dvec2(1 - fc, fc);
    }
    return sum / static_cast<double>(samples);
}

std::shared_ptr<const Image> bake_ibl_brdf(IblBrdfSettings settings, CancellationToken cancellation,
                                           IblProgressCallback progress) {
    cancellation.check();
    settings.validate();
    const auto total = settings.resolution * settings.resolution;
    report(progress, IblStage::brdf, 0, total, cancellation);
    Image lut(settings.resolution, settings.resolution);
    for (std::size_t y = 0; y < settings.resolution; ++y) {
        for (std::size_t x = 0; x < settings.resolution; ++x) {
            const auto value = integrate_ibl_brdf(
                static_cast<double>(x) / static_cast<double>(settings.resolution - 1),
                static_cast<double>(y) / static_cast<double>(settings.resolution - 1),
                settings.samples,
                settings.seed + (y * settings.resolution + x) * 0x9e3779b97f4a7c15ULL,
                cancellation);
            lut.at(x, y) = {static_cast<float>(value.x), static_cast<float>(value.y), 0};
        }
        report(progress, IblStage::brdf, (y + 1) * settings.resolution, total, cancellation);
    }
    report(progress, IblStage::complete, 1, 1, cancellation);
    return std::make_shared<const Image>(std::move(lut));
}

std::shared_ptr<const IblEnvironment> bake_ibl_environment(std::shared_ptr<const Texture> source,
                                                           IblEnvironmentSettings settings,
                                                           CancellationToken cancellation,
                                                           IblProgressCallback progress) {
    cancellation.check();
    settings.validate();
    if (!source) {
        throw std::invalid_argument("IBL bake requires a source texture");
    }
    source = prepare_source(std::move(source), settings.source_maximum_dimension, cancellation,
                            progress);
    Image diffuse(settings.diffuse_width, settings.diffuse_width / 2);
    const auto diffuse_total = diffuse.width() * diffuse.height();
    report(progress, IblStage::diffuse, 0, diffuse_total, cancellation);
    for (std::size_t y = 0; y < diffuse.height(); ++y) {
        for (std::size_t x = 0; x < diffuse.width(); ++x) {
            if (x != 0 && (y == 0 || y + 1 == diffuse.height())) {
                // 极点只有一个方向，复制整行避免不同经度的采样误差形成接缝。
                diffuse.at(x, y) = diffuse.at(0, y);
                continue;
            }
            const ShadingFrame frame(direction_at(x, y, diffuse));
            const auto shift = rotation(settings.seed, y * diffuse.width() + x, 1);
            glm::dvec3 sum(0);
            for (std::uint32_t i = 0; i < settings.diffuse_samples; ++i) {
                if ((i & 63U) == 0) {
                    cancellation.check();
                }
                const auto direction = frame.to_world(
                    sample_cosine_hemisphere(sequence(i, settings.diffuse_samples, shift)));
                sum += source_lookup(*source, direction, 0);
            }
            diffuse.at(x, y) = glm::vec3(sum / static_cast<double>(settings.diffuse_samples));
        }
        report(progress, IblStage::diffuse, (y + 1) * diffuse.width(), diffuse_total, cancellation);
    }
    std::vector<Image> specular;
    specular.reserve(settings.specular_layers);
    const auto layer_pixels = settings.specular_width * settings.specular_width / 2;
    const auto specular_total = layer_pixels * settings.specular_layers;
    report(progress, IblStage::specular, 0, specular_total, cancellation);
    for (std::size_t layer = 0; layer < settings.specular_layers; ++layer) {
        Image image(settings.specular_width, settings.specular_width / 2);
        const double roughness =
            static_cast<double>(layer) / static_cast<double>(settings.specular_layers - 1);
        const double alpha = roughness * roughness;
        for (std::size_t y = 0; y < image.height(); ++y) {
            for (std::size_t x = 0; x < image.width(); ++x) {
                if (x != 0 && (y == 0 || y + 1 == image.height())) {
                    image.at(x, y) = image.at(0, y);
                    continue;
                }
                const auto normal = direction_at(x, y, image);
                if (layer == 0) {
                    image.at(x, y) = glm::vec3(source_lookup(*source, normal, 0));
                    continue;
                }
                const ShadingFrame frame(normal);
                const auto shift = rotation(settings.seed, y * image.width() + x, 2 + layer);
                glm::dvec3 sum(0);
                double weights = 0;
                for (std::uint32_t i = 0; i < settings.specular_samples; ++i) {
                    if ((i & 63U) == 0) {
                        cancellation.check();
                    }
                    const auto u = sequence(i, settings.specular_samples, shift);
                    const double cosine = std::sqrt((1 - u.y) / (1 + (alpha * alpha - 1) * u.y));
                    const double sine = std::sqrt(std::max(0.0, 1 - cosine * cosine));
                    const glm::dvec3 halfway{sine * std::cos(2 * pi * u.x),
                                             sine * std::sin(2 * pi * u.x), cosine};
                    const auto local = 2 * cosine * halfway - glm::dvec3(0, 0, 1);
                    if (local.z <= 0) {
                        continue;
                    }
                    const auto direction = frame.to_world(local);
                    const double denominator = sine * sine + alpha * alpha * cosine * cosine;
                    // N=V 时 NdotH/(4*VdotH) 抵消为 1/4，反射方向密度为 D/4。
                    const double pdf = alpha * alpha / (4 * pi * denominator * denominator);
                    const double lod =
                        source_lod(*source, direction, pdf, settings.specular_samples);
                    sum += source_lookup(*source, direction, lod) * local.z;
                    weights += local.z;
                }
                if (weights == 0) {
                    throw std::runtime_error("IBL prefilter found no upper hemisphere samples");
                }
                image.at(x, y) = glm::vec3(sum / weights);
            }
            report(progress, IblStage::specular, layer * layer_pixels + (y + 1) * image.width(),
                   specular_total, cancellation);
        }
        specular.push_back(std::move(image));
    }
    report(progress, IblStage::complete, 1, 1, cancellation);
    return std::make_shared<const IblEnvironment>(
        IblEnvironment{std::move(source), std::move(diffuse), std::move(specular)});
}

} // namespace minirender
