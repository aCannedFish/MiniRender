#include <minirender/core/sampling.hpp>
#include <minirender/render/bsdf.hpp>
#include <minirender/render/cloth_bsdf.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace minirender {
namespace {

constexpr double pi = std::numbers::pi;
constexpr double radians = pi / 180;
constexpr double cosine_probability = .2;

bool finite(glm::dvec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
glm::dvec3 unit(glm::dvec3 value) {
    if (!finite(value) || std::abs(glm::dot(value, value) - 1) > 1e-4) {
        throw std::invalid_argument("Cloth BSDF direction must be finite and unit length");
    }
    return glm::normalize(value);
}
void check_sample(double value) {
    if (!(value >= 0 && value < 1)) {
        throw std::invalid_argument("Cloth sample coordinate must be in [0,1)");
    }
}

struct ThreadFrame {
    glm::dvec3 tangent;
    glm::dvec3 normal;
    glm::dvec3 side;
};
ThreadFrame frame_for(const ClothTangentSample& sample, glm::dvec3 axis) {
    const glm::dvec3 normal(0, 0, 1);
    return {sample.cosine * axis + sample.sine * normal,
            sample.cosine * normal - sample.sine * axis, glm::cross(normal, axis)};
}
struct ThreadAngles {
    double theta;
    double phi;
    double psi;
    double cosine;
    double sine;
    double normal;
};
ThreadAngles angles_for(const ThreadFrame& frame, glm::dvec3 direction) {
    const double t = glm::dot(direction, frame.tangent);
    const double n = glm::dot(direction, frame.normal);
    const double b = glm::dot(direction, frame.side);
    // 直接计算法平面投影，保留接近纱线轴时的小余弦。
    const double cosine = std::hypot(n, b);
    return {std::atan2(t, cosine), std::atan2(b, n), std::atan2(t, n), cosine, t, n};
}
double gaussian(double width, double angle) {
    const double x = angle / width;
    return std::exp(-.5 * x * x) / (std::sqrt(2 * pi) * width);
}
double correlated(double a, double b, double difference, double width) {
    const double x = difference / width;
    const double correlation = std::exp(-.5 * x * x);
    return (1 - correlation) * a * b + correlation * std::min(a, b);
}
double surface_probability(const ClothThread& thread, double ior, glm::dvec3 dye) {
    const double f0 = (ior - 1) * (ior - 1) / ((ior + 1) * (ior + 1));
    const double albedo = glm::dot(thread.albedo * dye, glm::dvec3(.2126, .7152, .0722));
    return std::clamp(f0 / (f0 + albedo + 1e-12), .1, .9);
}

// Wang et al. 2014：在有限纵向角区间归一化 Cauchy，近似 Gaussian 波瓣。
double longitudinal_pdf(double theta_i, double theta_o, double width) {
    const double low = std::atan((theta_o - pi / 2) / (2 * width));
    const double high = std::atan((theta_o + pi / 2) / (2 * width));
    const double half = .5 * (theta_i + theta_o);
    return width / (2 * (high - low) * (half * half + width * width));
}
double sample_longitudinal(double theta_o, double width, double u) {
    const double low = std::atan((theta_o - pi / 2) / (2 * width));
    const double high = std::atan((theta_o + pi / 2) / (2 * width));
    return std::clamp(2 * width * std::tan(low + u * (high - low)) - theta_o, -pi / 2, pi / 2);
}

} // namespace

ClothBsdf::ClothBsdf(std::shared_ptr<const ClothAppearance> appearance, double ior, glm::dvec3 dye,
                     ClothAxes axes)
    : appearance_(std::move(appearance)), ior_(ior), dye_(dye) {
    if (!appearance_ || !std::isfinite(ior) || ior < 1 || ior > 3 || !finite(dye) ||
        glm::any(glm::lessThan(dye, glm::dvec3(0))) ||
        glm::any(glm::greaterThan(dye, glm::dvec3(1)))) {
        throw std::invalid_argument("Invalid cloth appearance, IOR or dye");
    }
    const double rotation =
        std::remainder(appearance_->settings().rotation_degrees, 360.0) * radians;
    const double c = std::cos(rotation), s = std::sin(rotation);
    const std::array inputs{axes.u, axes.v};
    for (std::size_t i = 0; i < 2; ++i) {
        auto axis = inputs[i];
        const double scale = std::max(std::abs(axis.x), std::abs(axis.y));
        if (!finite(axis) || scale == 0) {
            throw std::invalid_argument("Cloth UV axis has no tangent-plane projection");
        }
        axis = glm::normalize(glm::dvec3(axis.x / scale, axis.y / scale, 0));
        axes_[i] = {c * axis.x - s * axis.y, s * axis.x + c * axis.y, 0};
    }
}

glm::dvec3 ClothBsdf::response_normalized(glm::dvec3 wo, glm::dvec3 wi) const {
    if (wo.z <= 0 || wi.z <= 0) {
        return glm::dvec3(0);
    }
    const auto& settings = appearance_->settings();
    const double width = settings.correlation_width_degrees * radians;
    double normalization = (1 - appearance_->coverage()) * wo.z;
    glm::dvec3 result(0);
    for (const auto& sample : appearance_->samples()) {
        if (sample.weight == 0) {
            continue;
        }
        const auto frame = frame_for(sample, axes_[sample.thread]);
        const auto in = angles_for(frame, wi), out = angles_for(frame, wo);
        if (in.normal <= 0 || out.normal <= 0) {
            continue;
        }
        const double phi_d = std::remainder(in.phi - out.phi, 2 * pi);
        const double masking =
            correlated(in.normal / in.cosine, out.normal / out.cosine, phi_d, width);
        const double projection =
            correlated(in.normal / std::hypot(in.sine, in.normal),
                       out.normal / std::hypot(out.sine, out.normal), in.psi - out.psi, width);
        normalization += sample.weight * projection;

        // 半角余弦由两个单位圆方向的和求得，避免相反方向处的 1+cos 消去。
        const double csum = in.cosine + out.cosine, ssum = in.sine + out.sine;
        const double cosine_d_squared = std::max(.25 * (csum * csum + ssum * ssum), 1e-12);
        const double cosine_phi = std::cos(.5 * phi_d);
        const double fresnel = fresnel_dielectric(
            std::clamp(std::sqrt(cosine_d_squared) * cosine_phi, 0.0, 1.0), 1, ior_);
        // 入/出射界面使用方向在圆柱法平面的投影法线；外部余弦分别为 cos(theta_i/o)。
        const double transmission =
            (1 - fresnel_dielectric(std::clamp(in.cosine, 0.0, 1.0), 1, ior_)) *
            (1 - fresnel_dielectric(std::clamp(out.cosine, 0.0, 1.0), 1, ior_));
        const auto& thread = settings.threads[sample.thread];
        const double theta_h = .5 * (in.theta + out.theta);
        const double surface =
            fresnel * cosine_phi * gaussian(thread.surface_width_degrees * radians, theta_h);
        const double volume =
            transmission *
            ((1 - thread.kd) * gaussian(thread.volume_width_degrees * radians, theta_h) +
             thread.kd) /
            (in.cosine + out.cosine);
        const auto scattering = glm::dvec3(surface) + volume * thread.albedo * dye_;
        result += sample.weight * masking * projection * in.cosine / cosine_d_squared * scattering;
    }
    return normalization > 1e-12 ? result / normalization : glm::dvec3(0);
}
glm::dvec3 ClothBsdf::response(glm::dvec3 wo, glm::dvec3 wi) const {
    return response_normalized(unit(wo), unit(wi));
}
glm::dvec3 ClothBsdf::evaluate(glm::dvec3 wo, glm::dvec3 wi) const {
    wo = unit(wo);
    wi = unit(wi);
    return wi.z > 0 ? response_normalized(wo, wi) / wi.z : glm::dvec3(0);
}

double ClothBsdf::pdf_normalized(glm::dvec3 wo, glm::dvec3 wi) const {
    if (wo.z <= 0 || wi.z <= 0) {
        return 0;
    }
    const auto& settings = appearance_->settings();
    double result = 0;
    for (const auto& sample : appearance_->samples()) {
        if (sample.weight == 0) {
            continue;
        }
        const auto frame = frame_for(sample, axes_[sample.thread]);
        const auto in = angles_for(frame, wi), out = angles_for(frame, wo);
        if (in.cosine == 0) {
            continue;
        }
        const auto& thread = settings.threads[sample.thread];
        const double phi_d = std::remainder(in.phi - out.phi, 2 * pi);
        const double azimuth = .25 * std::max(0.0, std::cos(.5 * phi_d));
        const double p_surface =
            longitudinal_pdf(in.theta, out.theta, thread.surface_width_degrees * radians) *
            azimuth / in.cosine;
        const double p_anisotropic =
            longitudinal_pdf(in.theta, out.theta, thread.volume_width_degrees * radians) * azimuth /
            in.cosine;
        const double p_isotropic = 1 / (4 * pi);
        const double surface = surface_probability(thread, ior_, dye_);
        result += sample.weight *
                  (surface * p_surface +
                   (1 - surface) * ((1 - thread.kd) * p_anisotropic + thread.kd * p_isotropic));
    }
    return cosine_probability * wi.z / pi +
           (1 - cosine_probability) * result / appearance_->coverage();
}
double ClothBsdf::pdf(glm::dvec3 wo, glm::dvec3 wi) const {
    return pdf_normalized(unit(wo), unit(wi));
}

std::optional<glm::dvec3> ClothBsdf::sample(glm::dvec3 wo, double component, glm::dvec2 u) const {
    wo = unit(wo);
    check_sample(component);
    check_sample(u.x);
    check_sample(u.y);
    if (wo.z <= 0) {
        return std::nullopt;
    }
    if (component < cosine_probability) {
        const auto direction = sample_cosine_hemisphere(u);
        return direction.z > 0 ? std::optional(direction) : std::nullopt;
    }
    const auto& settings = appearance_->settings();
    double selected = (component - cosine_probability) / (1 - cosine_probability);
    selected *= appearance_->coverage();
    std::uint32_t thread = selected < settings.coverage[0] ? 0U : 1U;
    if (thread == 1) {
        selected -= settings.coverage[0];
    }
    // 零覆盖方向不会被选择；舍入到区间末端时保留最后一个有效样本。
    if (settings.coverage[thread] == 0) {
        thread = 1 - thread;
        selected = std::nextafter(settings.coverage[thread], 0.0);
    }
    selected = std::clamp(selected / settings.coverage[thread], 0.0, std::nextafter(1.0, 0.0));
    selected *= settings.samples_per_thread;
    const auto index =
        std::min(static_cast<std::uint32_t>(selected), settings.samples_per_thread - 1);
    const double lobe = selected - index;
    const auto& tangent = appearance_->samples()[thread * settings.samples_per_thread + index];
    const auto frame = frame_for(tangent, axes_[thread]);
    const auto out = angles_for(frame, wo);
    const auto& parameters = settings.threads[thread];
    const double surface = surface_probability(parameters, ior_, dye_);
    double theta, phi;
    if (lobe < surface) {
        theta = sample_longitudinal(out.theta, parameters.surface_width_degrees * radians, u.x);
        phi = out.phi - 2 * std::asin(2 * u.y - 1);
    } else if (lobe < surface + (1 - surface) * (1 - parameters.kd)) {
        theta = sample_longitudinal(out.theta, parameters.volume_width_degrees * radians, u.x);
        phi = out.phi - 2 * std::asin(2 * u.y - 1);
    } else {
        theta = std::asin(2 * u.x - 1);
        phi = 2 * pi * u.y - pi;
    }
    const auto direction = glm::normalize(
        std::sin(theta) * frame.tangent +
        std::cos(theta) * (std::cos(phi) * frame.normal + std::sin(phi) * frame.side));
    return direction.z > 0 ? std::optional(direction) : std::nullopt;
}

} // namespace minirender
