#include <minirender/render/light.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace minirender {
namespace {

constexpr double pi = std::numbers::pi;
constexpr double uniform_fraction = 0.05;

bool finite(glm::dvec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
double luminance(glm::dvec3 color) {
    return glm::dot(color, glm::dvec3(0.2126, 0.7152, 0.0722));
}
glm::dvec3 positive(glm::dvec3 color) {
    return glm::max(color, glm::dvec3(0));
}
void check_unit(glm::dvec2 u) {
    if (!(u.x >= 0 && u.x < 1 && u.y >= 0 && u.y < 1)) {
        throw std::invalid_argument("Light sample coordinates must be in [0,1)");
    }
}
glm::dvec3 unit(glm::dvec3 direction) {
    if (!finite(direction) || std::abs(glm::dot(direction, direction) - 1) > 1e-4) {
        throw std::invalid_argument("Light direction must be finite and have unit length");
    }
    return glm::normalize(direction);
}
const SceneSnapshot& checked_scene(const std::shared_ptr<const SceneSnapshot>& scene) {
    if (!scene) {
        throw std::invalid_argument("Light sampler requires a scene snapshot");
    }
    return *scene;
}
double area_density(glm::dvec3 reference, glm::dvec3 point, const glm::dvec3& normal, double area) {
    const auto difference = point - reference;
    const double squared_distance = glm::dot(difference, difference);
    if (squared_distance == 0) {
        return 0;
    }
    const double cosine = std::abs(glm::dot(normal, -difference / std::sqrt(squared_distance)));
    return cosine > 0 ? squared_distance / (area * cosine) : 0;
}

} // namespace

EnvironmentLight::EnvironmentLight(const SceneSnapshot& scene, CancellationToken cancellation)
    : constant_(scene.environment), texture_(scene.environment_texture),
      strength_(scene.environment_strength),
      rotation_(std::remainder(static_cast<double>(scene.environment_rotation_degrees), 360.0) *
                pi / 180),
      integral_(4 * pi * luminance(constant_)) {
    cancellation.check();
    if (!texture_ || strength_ == 0) {
        return;
    }
    while (texture_->level(grid_level_).width() > 1024 ||
           texture_->level(grid_level_).height() > 512) {
        ++grid_level_;
    }
    const auto& image = texture_->level(grid_level_);
    std::vector<double> weights;
    weights.reserve(image.width() * image.height());
    solid_angles_.reserve(image.height());
    double map_integral = 0;
    for (std::size_t y = 0; y < image.height(); ++y) {
        cancellation.check();
        const double theta0 = pi * static_cast<double>(y) / static_cast<double>(image.height());
        const double theta1 = pi * static_cast<double>(y + 1) / static_cast<double>(image.height());
        const double angle = (2 * pi / static_cast<double>(image.width())) * 2 *
                             std::sin((theta0 + theta1) * 0.5) * std::sin((theta1 - theta0) * 0.5);
        solid_angles_.push_back(angle);
        for (std::size_t x = 0; x < image.width(); ++x) {
            const double weight = luminance(positive(glm::dvec3(image.at(x, y)))) * angle;
            weights.push_back(weight);
            map_integral += weight;
        }
    }
    pixels_ = DiscreteDistribution(weights);
    uniform_ = map_integral == 0;
    // mip 在负值 EXR 中可能先把正负辐射抵消；选择功率从原图的非负辐射估计，
    // 保证原图仍有照明时不会从光源列表中删掉环境。CDF 为零时使用均匀球面分布。
    double full_integral = 0;
    const auto& full = texture_->level(0);
    for (std::size_t y = 0; y < full.height(); ++y) {
        cancellation.check();
        const double theta0 = pi * static_cast<double>(y) / static_cast<double>(full.height());
        const double theta1 = pi * static_cast<double>(y + 1) / static_cast<double>(full.height());
        const double angle = (2 * pi / static_cast<double>(full.width())) * 2 *
                             std::sin((theta0 + theta1) * 0.5) * std::sin((theta1 - theta0) * 0.5);
        for (std::size_t x = 0; x < full.width(); ++x) {
            full_integral += luminance(positive(glm::dvec3(full.at(x, y)))) * angle;
        }
    }
    integral_ += strength_ * full_integral;
}

glm::dvec2 EnvironmentLight::coordinates(glm::dvec3 direction) const {
    direction = unit(direction);
    double u = (std::atan2(direction.z, direction.x) + rotation_) / (2 * pi);
    u -= std::floor(u);
    // atan2 保留极点附近 X/Z 的信息，不依赖可能已经舍入为 ±1 的 Y 分量。
    const double v = std::atan2(std::hypot(direction.x, direction.z), direction.y) / pi;
    return {u, v};
}
glm::dvec3 EnvironmentLight::lookup(glm::dvec2 uv) const {
    const auto& image = texture_->level(0);
    const double x = uv.x * static_cast<double>(image.width()) - 0.5;
    const double y = uv.y * static_cast<double>(image.height()) - 0.5;
    const int ix = static_cast<int>(std::floor(x));
    const int iy = static_cast<int>(std::floor(y));
    const double tx = x - std::floor(x), ty = y - std::floor(y);
    const auto texel = [&](int column, int row) {
        const int width = static_cast<int>(image.width()),
                  height = static_cast<int>(image.height());
        const auto wrapped = static_cast<std::size_t>((column % width + width) % width);
        return positive(glm::dvec3(
            image.at(wrapped, static_cast<std::size_t>(std::clamp(row, 0, height - 1)))));
    };
    return glm::mix(glm::mix(texel(ix, iy), texel(ix + 1, iy), tx),
                    glm::mix(texel(ix, iy + 1), texel(ix + 1, iy + 1), tx), ty);
}
glm::dvec3 EnvironmentLight::evaluate(glm::dvec3 direction) const {
    const auto uv = coordinates(direction);
    return constant_ + (texture_ ? strength_ * lookup(uv) : glm::dvec3(0));
}
double EnvironmentLight::pdf(glm::dvec3 direction) const {
    const auto uv = coordinates(direction);
    if (uniform_) {
        return 1 / (4 * pi);
    }
    const auto& grid = texture_->level(grid_level_);
    const auto x = std::min(static_cast<std::size_t>(uv.x * static_cast<double>(grid.width())),
                            grid.width() - 1);
    const auto y = std::min(static_cast<std::size_t>(uv.y * static_cast<double>(grid.height())),
                            grid.height() - 1);
    return uniform_fraction / (4 * pi) +
           (1 - uniform_fraction) * pixels_.probability(y * grid.width() + x) / solid_angles_[y];
}
EnvironmentSample EnvironmentLight::sample(glm::dvec2 u) const {
    check_unit(u);
    glm::dvec3 direction;
    if (uniform_ || u.x < uniform_fraction) {
        direction = sample_sphere({uniform_ ? u.x : u.x / uniform_fraction, u.y});
    } else {
        const auto selected = pixels_.sample((u.x - uniform_fraction) / (1 - uniform_fraction));
        const auto& grid = texture_->level(grid_level_);
        const auto x = selected->index % grid.width(), y = selected->index / grid.width();
        const double theta0 = pi * static_cast<double>(y) / static_cast<double>(grid.height());
        const double theta1 = pi * static_cast<double>(y + 1) / static_cast<double>(grid.height());
        const double difference =
            2 * std::sin((theta0 + theta1) * 0.5) * std::sin((theta1 - theta0) * 0.5);
        const double v =
            std::clamp(u.y, std::numeric_limits<double>::epsilon() * 0.5, std::nextafter(1.0, 0.0));
        double cosine, sine;
        if (y < grid.height() / 2) {
            const double h = 1 - std::cos(theta0) + v * difference;
            cosine = 1 - h;
            sine = std::sqrt(std::max(0.0, h * (2 - h)));
        } else {
            const double h = 1 + std::cos(theta1) + (1 - v) * difference;
            cosine = -1 + h;
            sine = std::sqrt(std::max(0.0, h * (2 - h)));
        }
        const double phi = 2 * pi * (static_cast<double>(x) + selected->remapped) /
                               static_cast<double>(grid.width()) -
                           rotation_;
        direction = {sine * std::cos(phi), cosine, sine * std::sin(phi)};
    }
    return {direction, evaluate(direction), pdf(direction)};
}
double EnvironmentLight::luminance_integral() const noexcept {
    return integral_;
}

LightSampler::LightSampler(std::shared_ptr<const SceneSnapshot> scene,
                           CancellationToken cancellation)
    : scene_(std::move(scene)), environment_(checked_scene(scene_), cancellation) {
    cancellation.check();
    Bounds3 bounds;
    std::vector<double> weights;
    for (std::size_t index = 0; index < scene_->instances.size(); ++index) {
        const auto& instance = scene_->instances[index];
        const auto radiance = glm::dvec3(instance.material.emission);
        for (std::size_t face = 0; face < instance.mesh->triangles.size(); ++face) {
            if ((face & 255U) == 0) {
                cancellation.check();
            }
            std::array<glm::dvec3, 3> vertices;
            for (std::size_t corner = 0; corner < 3; ++corner) {
                const auto local =
                    instance.mesh
                        ->positions[instance.mesh->triangles[face].corners[corner].position];
                const auto world = glm::vec3(instance.object_to_world * glm::vec4(local, 1));
                if (!finite(glm::dvec3(world))) {
                    throw std::invalid_argument("Transformed light scene position is not finite");
                }
                bounds.expand(world);
                vertices[corner] = world;
            }
            if (luminance(radiance) == 0) {
                continue;
            }
            const auto cross = glm::cross(vertices[1] - vertices[0], vertices[2] - vertices[0]);
            const double area = glm::length(cross) * 0.5;
            if (area == 0) {
                continue;
            }
            area_choices_[{static_cast<std::uint32_t>(index), static_cast<std::uint32_t>(face)}] =
                choices_.size();
            choices_.push_back({LightKind::area, areas_.size()});
            // 镜像后的外侧与 BVH 一致；单面发光只覆盖外侧半球。
            const double orientation =
                glm::determinant(glm::dmat3(instance.object_to_world)) < 0 ? -1 : 1;
            areas_.push_back({vertices, cross * orientation / (2 * area), radiance, area,
                              instance.material.double_sided});
            weights.push_back((instance.material.double_sided ? 2 : 1) * pi * area *
                              luminance(radiance));
        }
    }
    // 投影包围球面积用于估计无限光穿过场景的功率；只影响选择效率，最终 PMF 始终计入 PDF。
    const auto extent = glm::dvec3(bounds.maximum) - glm::dvec3(bounds.minimum);
    const double projected_area =
        pi * (bounds.empty() ? 1 : std::max(glm::dot(extent, extent) * 0.25, 1e-12));
    emission_radius_ = std::sqrt(projected_area / pi);
    if (!bounds.empty()) {
        emission_center_ = glm::dvec3(bounds.minimum) + extent * .5;
    }
    for (const auto& light : scene_->directional_lights) {
        const auto radiance = glm::dvec3(light.color) * static_cast<double>(light.intensity);
        if (luminance(radiance) > 0) {
            choices_.push_back({LightKind::directional, directional_.size()});
            directional_.push_back({-glm::normalize(glm::dvec3(light.direction)), radiance});
            weights.push_back(projected_area * luminance(radiance));
        }
    }
    if (environment_.luminance_integral() > 0) {
        environment_choice_ = choices_.size();
        choices_.push_back({LightKind::environment, 0});
        weights.push_back(projected_area * environment_.luminance_integral());
    }
    selection_ = DiscreteDistribution(weights);
}
std::optional<LightSample> LightSampler::sample(glm::dvec3 reference, double light_u,
                                                glm::dvec2 surface_u) const {
    if (!finite(reference)) {
        throw std::invalid_argument("Light reference point must be finite");
    }
    check_unit(surface_u);
    const auto selected = selection_.sample(light_u);
    if (!selected) {
        return std::nullopt;
    }
    const auto& choice = choices_[selected->index];
    if (choice.kind == LightKind::directional) {
        const auto& light = directional_[choice.index];
        return LightSample{
            light.direction,       light.radiance,          std::numeric_limits<double>::infinity(),
            selected->probability, SampleMeasure::discrete, choice.kind,
            selected->index};
    }
    if (choice.kind == LightKind::environment) {
        const auto sample = environment_.sample(surface_u);
        return LightSample{sample.direction,
                           sample.radiance,
                           std::numeric_limits<double>::infinity(),
                           selected->probability * sample.probability,
                           SampleMeasure::solid_angle,
                           choice.kind,
                           selected->index};
    }
    const auto& light = areas_[choice.index];
    const auto barycentric = sample_triangle(surface_u);
    const auto point = barycentric.x * light.vertices[0] + barycentric.y * light.vertices[1] +
                       barycentric.z * light.vertices[2];
    if (!light.double_sided && glm::dot(light.normal, reference - point) <= 0) {
        return std::nullopt;
    }
    const double density = area_density(reference, point, light.normal, light.area);
    if (density == 0) {
        return std::nullopt;
    }
    const auto direction = point - reference;
    const double distance = glm::length(direction);
    const auto error =
        (glm::abs(barycentric.x * light.vertices[0]) + glm::abs(barycentric.y * light.vertices[1]) +
         glm::abs(barycentric.z * light.vertices[2])) *
        (7 * std::numeric_limits<double>::epsilon());
    return LightSample{direction / distance,
                       light.radiance,
                       distance,
                       selected->probability * density,
                       SampleMeasure::solid_angle,
                       choice.kind,
                       selected->index,
                       point,
                       light.normal,
                       error};
}
double LightSampler::area_pdf(glm::dvec3 reference, const SurfaceHit& hit) const {
    const auto choice = area_choices_.find({hit.instance_index, hit.triangle_index});
    if (choice == area_choices_.end()) {
        return 0;
    }
    const auto& light = areas_[choices_[choice->second].index];
    if (!light.double_sided && glm::dot(light.normal, reference - hit.position) <= 0) {
        return 0;
    }
    return selection_.probability(choice->second) *
           area_density(reference, hit.position, light.normal, light.area);
}
double LightSampler::environment_pdf(glm::dvec3 direction) const {
    return environment_choice_
               ? selection_.probability(*environment_choice_) * environment_.pdf(direction)
               : 0;
}
const EnvironmentLight& LightSampler::environment() const noexcept {
    return environment_;
}
std::size_t LightSampler::size() const noexcept {
    return choices_.size();
}
double LightSampler::selection_probability(std::size_t emitter) const {
    return selection_.probability(emitter);
}

} // namespace minirender
