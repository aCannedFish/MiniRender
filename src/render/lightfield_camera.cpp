#include <minirender/render/lightfield.hpp>

#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace minirender {
namespace {
void grid(std::uint32_t columns, std::uint32_t rows) {
    if (columns < 3 || columns > 15 || rows < 3 || rows > 15 || columns % 2 == 0 || rows % 2 == 0) {
        throw std::invalid_argument("Light field grid dimensions must be odd integers in 3..15");
    }
}
void optics(double radius, double focus) {
    if (!std::isfinite(radius) || radius <= 0 || radius > 1e6 || !std::isfinite(focus) ||
        focus < 1e-6 || focus > 1e6) {
        throw std::invalid_argument(
            "Light field requires a positive pupil radius and reference focus in 1e-6..1e6");
    }
}
} // namespace

std::vector<glm::dvec2> lightfield_pupils(std::uint32_t columns, std::uint32_t rows,
                                          double radius) {
    grid(columns, rows);
    optics(radius, 1);
    std::vector<glm::dvec2> result;
    result.reserve(std::size_t(columns) * rows);
    for (std::uint32_t y = 0; y < rows; ++y) {
        for (std::uint32_t x = 0; x < columns; ++x) {
            result.push_back(sample_disk({(x + .5) / columns, 1 - (y + .5) / rows}) * radius);
        }
    }
    return result;
}
std::size_t LightFieldCalibration::view_count() const noexcept {
    return std::size_t(columns) * rows;
}
std::size_t LightFieldCalibration::center_view() const noexcept {
    return std::size_t(rows / 2) * columns + columns / 2;
}
std::uint64_t LightFieldCalibration::view_bytes() const {
    render.validate();
    return std::uint64_t(render.width) * render.height * 3 * sizeof(float);
}
std::uint64_t LightFieldCalibration::total_paths() const {
    render.validate();
    grid(columns, rows);
    const auto pixels = std::uint64_t(render.width) * render.height;
    if (pixels >
        std::numeric_limits<std::uint64_t>::max() / render.samples_per_pixel / view_count()) {
        throw std::length_error("Light field sample count overflows uint64");
    }
    return pixels * render.samples_per_pixel * view_count();
}
double LightFieldCalibration::focal_pixels() const {
    return double(render.height) /
           (2 * std::tan(glm::radians(double(camera.vertical_fov_degrees)) / 2));
}
void LightFieldCalibration::validate() const {
    camera.validate();
    render.validate();
    integrator.validate();
    if (integrator.kind != IntegratorKind::path_tracing) {
        throw std::invalid_argument("Light field acquisition uses the path tracing backend");
    }
    grid(columns, rows);
    optics(pupil_radius, reference_focus);
    if (pupils.size() != view_count() || pupils[center_view()] != glm::dvec2(0)) {
        throw std::invalid_argument(
            "Light field requires a complete pupil grid with an exact center view");
    }
    std::set<std::pair<double, double>> unique;
    for (const auto pupil : pupils) {
        if (!std::isfinite(pupil.x) || !std::isfinite(pupil.y) ||
            glm::length(pupil) > pupil_radius * (1 + 1e-12) ||
            !unique.emplace(pupil.x, pupil.y).second) {
            throw std::invalid_argument(
                "Light field pupil positions must be unique finite points inside the aperture");
        }
    }
    if (view_bytes() * view_count() > 64ULL * 1024 * 1024 * 1024) {
        throw std::length_error("Light field exceeds the 64 GiB file payload limit");
    }
    total_paths();
}
Ray lightfield_ray(const LightFieldCalibration& calibration, std::size_t view, glm::dvec2 uv) {
    if (view >= calibration.pupils.size()) {
        throw std::out_of_range("Light field view index is out of range");
    }
    const double aspect = double(calibration.render.width) / double(calibration.render.height);
    const auto pinhole = calibration.camera.generate_ray(uv, aspect);
    const auto pupil = calibration.pupils[view];
    if (pupil == glm::dvec2(0)) {
        return pinhole;
    }
    const auto forward = glm::normalize(glm::dvec3(calibration.camera.target) -
                                        glm::dvec3(calibration.camera.position));
    const auto right = glm::normalize(glm::cross(forward, glm::dvec3(calibration.camera.up)));
    const auto up = glm::cross(right, forward);
    const auto focus = pinhole.origin + pinhole.direction * (calibration.reference_focus /
                                                             glm::dot(pinhole.direction, forward));
    const auto origin = pinhole.origin + pupil.x * right + pupil.y * up;
    return {origin, glm::normalize(focus - origin)};
}
glm::dvec2 lightfield_shift(const LightFieldCalibration& calibration, std::size_t view,
                            double focus) {
    if (view >= calibration.pupils.size() || !std::isfinite(focus) || focus < 1e-6 || focus > 1e6) {
        throw std::invalid_argument("Invalid light field view or refocus distance");
    }
    const double factor =
        calibration.focal_pixels() * (1 / calibration.reference_focus - 1 / focus);
    const auto pupil = calibration.pupils[view];
    return {factor * pupil.x, -factor * pupil.y};
}

} // namespace minirender
