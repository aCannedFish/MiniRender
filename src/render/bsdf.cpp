#include <minirender/core/sampling.hpp>
#include <minirender/render/bsdf.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace minirender {
namespace {

constexpr double pi = std::numbers::pi;
constexpr double delta_alpha = 1e-6;

bool finite(glm::dvec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
glm::dvec3 unit_direction(glm::dvec3 value) {
    if (!finite(value) || std::abs(glm::dot(value, value) - 1) > 1e-4) {
        throw std::invalid_argument("BSDF direction must be finite and have unit length");
    }
    return glm::normalize(value);
}
void check_sample(double value) {
    if (!(value >= 0 && value < 1)) {
        throw std::invalid_argument("BSDF sample coordinate must be in [0,1)");
    }
}
double distribution(glm::dvec3 m, double alpha) {
    const double a2 = alpha * alpha;
    // sin^2(theta) 直接由 x/y 分量得到，避免低粗糙度时 1-z^2 的消去误差。
    const double denominator = m.x * m.x + m.y * m.y + a2 * m.z * m.z;
    return m.z > 0 ? a2 / (pi * denominator * denominator) : 0;
}
double smith_g1_over_cosine(glm::dvec3 v, double alpha) {
    if (v.z <= 0) {
        return 0;
    }
    return 2 / (v.z + std::sqrt(v.z * v.z + alpha * alpha * (v.x * v.x + v.y * v.y)));
}

// 折射方向和 Fresnel 共享同一个 sin^2(theta_t)，保持临界角附近的事件选择一致。
double fresnel_from_transmission(double cosine, double sin2, double eta_i, double eta_t) {
    if (eta_i == eta_t) {
        return 0;
    }
    if (sin2 >= 1) {
        return 1;
    }
    const double transmitted_cosine = std::sqrt(1 - sin2);
    const double parallel = (eta_t * cosine - eta_i * transmitted_cosine) /
                            (eta_t * cosine + eta_i * transmitted_cosine);
    const double perpendicular = (eta_i * cosine - eta_t * transmitted_cosine) /
                                 (eta_i * cosine + eta_t * transmitted_cosine);
    return 0.5 * (parallel * parallel + perpendicular * perpendicular);
}
glm::dvec3 schlick(glm::dvec3 f0, double cosine) {
    const double t = 1 - std::clamp(cosine, 0.0, 1.0);
    return f0 + (1.0 - f0) * (t * t * t * t * t);
}

// Heitz 2018 GGX VNDF：拉伸 wo，采样可见半球投影，再恢复法线分布。
glm::dvec3 sample_visible_normal(glm::dvec3 wo, glm::dvec2 u, double alpha) {
    const auto view = glm::normalize(glm::dvec3(alpha * wo.x, alpha * wo.y, wo.z));
    const double lensq = view.x * view.x + view.y * view.y;
    const auto t1 =
        lensq > 0 ? glm::dvec3(-view.y, view.x, 0) / std::sqrt(lensq) : glm::dvec3(1, 0, 0);
    const auto t2 = glm::cross(view, t1);
    const double radius = std::sqrt(u.x);
    const double x = radius * std::cos(2 * pi * u.y);
    const double blend = 0.5 * (1 + view.z);
    const double y =
        (1 - blend) * std::sqrt(std::max(0.0, 1 - x * x)) + blend * radius * std::sin(2 * pi * u.y);
    const auto normal = x * t1 + y * t2 + std::sqrt(std::max(0.0, 1 - x * x - y * y)) * view;
    return glm::normalize(glm::dvec3(alpha * normal.x, alpha * normal.y, std::max(0.0, normal.z)));
}

} // namespace

ShadingFrame::ShadingFrame(glm::dvec3 normal) {
    const double maximum = std::max({std::abs(normal.x), std::abs(normal.y), std::abs(normal.z)});
    if (!finite(normal) || maximum == 0) {
        throw std::invalid_argument("Shading normal must be finite and nonzero");
    }
    normal_ = glm::normalize(normal / maximum);
    const auto helper = std::abs(normal_.z) < 0.999 ? glm::dvec3(0, 0, 1) : glm::dvec3(0, 1, 0);
    tangent_ = glm::normalize(glm::cross(helper, normal_));
    bitangent_ = glm::cross(normal_, tangent_);
}
glm::dvec3 ShadingFrame::to_local(glm::dvec3 world) const noexcept {
    return {glm::dot(world, tangent_), glm::dot(world, bitangent_), glm::dot(world, normal_)};
}
glm::dvec3 ShadingFrame::to_world(glm::dvec3 local) const noexcept {
    return local.x * tangent_ + local.y * bitangent_ + local.z * normal_;
}
const glm::dvec3& ShadingFrame::normal() const noexcept {
    return normal_;
}

double fresnel_dielectric(double cosine, double eta_i, double eta_t) {
    if (!std::isfinite(cosine) || cosine < 0 || cosine > 1 || !std::isfinite(eta_i) ||
        !std::isfinite(eta_t) || eta_i <= 0 || eta_t <= 0) {
        throw std::invalid_argument("Fresnel requires cosine in [0,1] and positive finite IORs");
    }
    const double ratio = eta_i / eta_t;
    const double sin2 = ratio * ratio * std::max(0.0, 1 - cosine * cosine);
    return fresnel_from_transmission(cosine, sin2, eta_i, eta_t);
}

Bsdf::Bsdf(const Material& material, bool front_face, glm::vec3 texture_color, ClothAxes cloth_axes)
    : kind_(material.kind),
      color_(glm::dvec3(material.base_color) * glm::clamp(glm::dvec3(texture_color), 0.0, 1.0)),
      alpha_(static_cast<double>(material.roughness) * material.roughness),
      eta_i_(front_face ? 1 : material.ior), eta_t_(front_face ? material.ior : 1) {
    material.validate();
    if (!finite(glm::dvec3(texture_color))) {
        throw std::invalid_argument("BSDF texture color must be finite");
    }
    if (kind_ == MaterialKind::cloth) {
        cloth_.emplace(material.cloth, material.ior, color_, cloth_axes);
    }
}
bool Bsdf::is_delta() const noexcept {
    return kind_ == MaterialKind::mirror || kind_ == MaterialKind::dielectric ||
           (kind_ == MaterialKind::conductor && alpha_ <= delta_alpha);
}
glm::dvec3 Bsdf::evaluate_normalized(glm::dvec3 wo, glm::dvec3 wi) const {
    if (wo.z <= 0 || wi.z <= 0 || is_delta()) {
        return glm::dvec3(0);
    }
    if (kind_ == MaterialKind::diffuse) {
        return color_ / pi;
    }
    if (cloth_) {
        return cloth_->evaluate(wo, wi);
    }
    const auto m = glm::normalize(wo + wi);
    // 约去 G1 中的余弦，避免掠射角下先计算极小乘积再相除。
    return schlick(color_, glm::dot(wo, m)) * distribution(m, alpha_) *
           smith_g1_over_cosine(wo, alpha_) * smith_g1_over_cosine(wi, alpha_) / 4.0;
}
double Bsdf::pdf_normalized(glm::dvec3 wo, glm::dvec3 wi) const {
    if (wo.z <= 0 || wi.z <= 0 || is_delta()) {
        return 0;
    }
    if (kind_ == MaterialKind::diffuse) {
        return cosine_hemisphere_pdf(wi);
    }
    if (cloth_) {
        return cloth_->pdf(wo, wi);
    }
    const auto m = glm::normalize(wo + wi);
    // p_m = D(m) G1(wo) |wo.m| / cos(wo)；反射 Jacobian 为 1/(4 |wo.m|)。
    return distribution(m, alpha_) * smith_g1_over_cosine(wo, alpha_) / 4;
}
glm::dvec3 Bsdf::evaluate(glm::dvec3 wo, glm::dvec3 wi) const {
    return evaluate_normalized(unit_direction(wo), unit_direction(wi));
}
double Bsdf::pdf(glm::dvec3 wo, glm::dvec3 wi) const {
    return pdf_normalized(unit_direction(wo), unit_direction(wi));
}
std::optional<BsdfSample> Bsdf::sample(glm::dvec3 wo, double component, glm::dvec2 u,
                                       TransportMode mode) const {
    wo = unit_direction(wo);
    check_sample(component);
    check_sample(u.x);
    check_sample(u.y);
    if (wo.z <= 0) {
        return std::nullopt;
    }
    const glm::dvec3 reflected{-wo.x, -wo.y, wo.z};
    if (cloth_) {
        const auto wi = cloth_->sample(wo, component, u);
        if (!wi) {
            return std::nullopt;
        }
        const double density = cloth_->pdf(wo, *wi);
        return BsdfSample{*wi,
                          cloth_->response(wo, *wi) / density,
                          density,
                          SampleMeasure::solid_angle,
                          ScatteringEvent::glossy_reflection,
                          1};
    }
    if (kind_ == MaterialKind::dielectric) {
        const double ratio = eta_i_ / eta_t_;
        const double sin2 = ratio * ratio * (wo.x * wo.x + wo.y * wo.y);
        const double reflectance =
            fresnel_from_transmission(std::clamp(wo.z, 0.0, 1.0), sin2, eta_i_, eta_t_);
        if (component < reflectance) {
            return BsdfSample{reflected,
                              glm::dvec3(1),
                              reflectance,
                              SampleMeasure::discrete,
                              ScatteringEvent::specular_reflection,
                              1};
        }
        const auto transmitted =
            glm::dvec3(-ratio * wo.x, -ratio * wo.y, -std::sqrt(std::max(0.0, 1 - sin2)));
        const double scale = mode == TransportMode::radiance ? ratio * ratio : 1;
        return BsdfSample{transmitted,
                          color_ * scale,
                          1 - reflectance,
                          SampleMeasure::discrete,
                          ScatteringEvent::specular_transmission,
                          eta_t_ / eta_i_};
    }
    if (is_delta()) {
        const auto weight = kind_ == MaterialKind::mirror ? color_ : schlick(color_, wo.z);
        return BsdfSample{
            reflected, weight, 1, SampleMeasure::discrete, ScatteringEvent::specular_reflection, 1};
    }
    glm::dvec3 wi;
    if (kind_ == MaterialKind::diffuse) {
        wi = sample_cosine_hemisphere(u);
    } else {
        const auto m = sample_visible_normal(wo, u, alpha_);
        wi = glm::normalize(2 * glm::dot(wo, m) * m - wo);
    }
    if (wi.z <= 0) {
        return std::nullopt;
    }
    // 与公开 eval/pdf 使用同一方向归一化，避免窄高光放大一次舍入差异。
    const auto evaluated_wi = unit_direction(wi);
    const double density = pdf_normalized(wo, evaluated_wi);
    const auto weight = evaluate_normalized(wo, evaluated_wi) * wi.z / density;
    return BsdfSample{wi,
                      weight,
                      density,
                      SampleMeasure::solid_angle,
                      kind_ == MaterialKind::diffuse ? ScatteringEvent::diffuse_reflection
                                                     : ScatteringEvent::glossy_reflection,
                      1};
}

} // namespace minirender
