#include "cloth_collision.hpp"
#include <minirender/simulation/cloth.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace minirender {
namespace {

bool finite(glm::dvec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::invalid_argument(message);
    }
}
bool range(double value, double low, double high) {
    return std::isfinite(value) && value >= low && value <= high;
}

// 预处理平面后，碰撞查询中的距离始终以米为单位。
ClothEnvironment normalized_environment(ClothEnvironment environment) {
    environment.validate();
    for (auto& plane : environment.planes) {
        plane.normal = glm::normalize(plane.normal);
    }
    return environment;
}

} // namespace

void ClothGrid::validate() const {
    require(columns >= 2 && columns <= 256 && rows >= 2 && rows <= 256,
            "Cloth grid dimensions must be in 2..256");
    const glm::dvec3 u(span_u), v(span_v);
    require(finite(glm::dvec3(origin)) && finite(u) && finite(v) &&
                glm::all(glm::lessThanEqual(glm::abs(glm::dvec3(origin)), glm::dvec3(1e6))),
            "Cloth grid coordinates must be finite and within the working range");
    constexpr double minimum_length = .01F;
    require(range(glm::length(u), minimum_length, 1000) &&
                range(glm::length(v), minimum_length, 1000),
            "Cloth grid edge lengths must be in 0.01..1000 meters");
    require(glm::length(glm::cross(u, v)) > 1e-6 * glm::length(u) * glm::length(v),
            "Cloth grid edges must be independent");
    std::set<std::uint32_t> seen;
    for (const auto index : pinned) {
        require(index < columns * rows && seen.insert(index).second,
                "Cloth fixed indices must be valid and unique");
    }
}

void ClothParameters::validate() const {
    require(range(surface_density, 1e-6, 1e4),
            "Cloth surface density must be in 1e-6..10000 kg/m2");
    require(range(structural_stiffness, 0, 1e8) && range(shear_stiffness, 0, 1e8) &&
                range(bending_stiffness, 0, 1e8),
            "Cloth stiffness must be in 0..1e8 N/m");
    require(range(spring_damping_ratio, 0, 1) && range(velocity_damping, 0, 100),
            "Cloth damping is outside the supported range");
    require(range(thickness, 0, 1) && range(frame_seconds, 1e-6, .1) && substeps > 0 &&
                substeps <= 4096,
            "Invalid cloth thickness, frame interval or substeps");
}

void ClothEnvironment::validate() const {
    require(finite(gravity) && finite(wind_velocity) &&
                glm::all(glm::lessThanEqual(glm::abs(gravity), glm::dvec3(1e4))) &&
                glm::all(glm::lessThanEqual(glm::abs(wind_velocity), glm::dvec3(1e4))),
            "Cloth gravity and wind must be finite and within the working range");
    require(range(air_density, 0, 100) && range(drag_coefficient, 0, 100),
            "Cloth air density and drag coefficient must be in 0..100");
    require(planes.size() <= 32 && spheres.size() <= 32,
            "Cloth supports up to 32 planes and spheres");
    for (const auto& plane : planes) {
        const double length = glm::length(plane.normal);
        require(finite(plane.point) && finite(plane.normal) && std::isfinite(length) &&
                    length > 1e-12 && range(plane.friction, 0, 1),
                "Invalid cloth collision plane");
    }
    for (const auto& sphere : spheres) {
        require(finite(sphere.center) && range(sphere.radius, 1e-6, 1e6) &&
                    range(sphere.friction, 0, 1),
                "Invalid cloth collision sphere");
    }
}

glm::dvec3 cloth_spring_force(const ClothSpring& spring, glm::dvec3 a, glm::dvec3 b,
                              glm::dvec3 velocity_a, glm::dvec3 velocity_b) {
    const auto delta = b - a;
    const double length = glm::length(delta);
    const auto direction =
        length > 1e-12 * spring.rest_length ? delta / length : spring.rest_direction;
    const double relative_speed = glm::dot(velocity_b - velocity_a, direction);
    return (spring.stiffness * (length - spring.rest_length) + spring.damping * relative_speed) *
           direction;
}

ClothSimulation::ClothSimulation(ClothGrid grid, ClothParameters parameters,
                                 ClothEnvironment environment, CancellationToken cancellation)
    : grid_(std::move(grid)), parameters_(parameters),
      environment_(normalized_environment(std::move(environment))) {
    cancellation.check();
    grid_.validate();
    parameters_.validate();
    const auto count = std::size_t(grid_.columns) * grid_.rows;
    rest_.reserve(count);
    masses_.resize(count, 0);
    pinned_.resize(count, false);
    for (const auto index : grid_.pinned) {
        pinned_[index] = true;
    }
    const double cell_area =
        glm::length(glm::cross(glm::dvec3(grid_.span_u), glm::dvec3(grid_.span_v))) /
        ((grid_.columns - 1.0) * (grid_.rows - 1.0));
    for (std::uint32_t row = 0; row < grid_.rows; ++row) {
        cancellation.check();
        for (std::uint32_t column = 0; column < grid_.columns; ++column) {
            rest_.push_back(glm::dvec3(grid_.origin) +
                            glm::dvec3(grid_.span_u) * (column / (grid_.columns - 1.0)) +
                            glm::dvec3(grid_.span_v) * (row / (grid_.rows - 1.0)));
        }
    }
    // 每个三角形将静止质量均分到三个端点，与空气压力的离散方式一致。
    for (std::uint32_t row = 0; row + 1 < grid_.rows; ++row) {
        cancellation.check();
        for (std::uint32_t column = 0; column + 1 < grid_.columns; ++column) {
            const auto a = row * grid_.columns + column;
            const auto b = a + grid_.columns;
            for (const auto face : {std::array{a, a + 1, b + 1}, std::array{a, b + 1, b}}) {
                triangles_.push_back(face);
                for (const auto index : face) {
                    masses_[index] += parameters_.surface_density * cell_area / 6;
                }
            }
        }
    }
    const auto spring = [&](std::uint32_t a, std::uint32_t b, ClothSpringKind kind,
                            double stiffness) {
        const auto delta = rest_[b] - rest_[a];
        const double length = glm::length(delta);
        const double inverse_mass =
            (pinned_[a] ? 0 : 1 / masses_[a]) + (pinned_[b] ? 0 : 1 / masses_[b]);
        const double damping = inverse_mass > 0 ? 2 * parameters_.spring_damping_ratio *
                                                      std::sqrt(stiffness / inverse_mass)
                                                : 0;
        springs_.push_back({a, b, kind, length, stiffness, damping, delta / length});
    };
    for (std::uint32_t row = 0; row < grid_.rows; ++row) {
        cancellation.check();
        for (std::uint32_t column = 0; column < grid_.columns; ++column) {
            const auto a = row * grid_.columns + column;
            if (column + 1 < grid_.columns) {
                spring(a, a + 1, ClothSpringKind::structural, parameters_.structural_stiffness);
            }
            if (row + 1 < grid_.rows) {
                spring(a, a + grid_.columns, ClothSpringKind::structural,
                       parameters_.structural_stiffness);
            }
            if (column + 1 < grid_.columns && row + 1 < grid_.rows) {
                const auto b = a + grid_.columns;
                spring(a, b + 1, ClothSpringKind::shear, parameters_.shear_stiffness);
                spring(a + 1, b, ClothSpringKind::shear, parameters_.shear_stiffness);
            }
            if (column + 2 < grid_.columns) {
                spring(a, a + 2, ClothSpringKind::bending, parameters_.bending_stiffness);
            }
            if (row + 2 < grid_.rows) {
                spring(a, a + 2 * grid_.columns, ClothSpringKind::bending,
                       parameters_.bending_stiffness);
            }
        }
    }

    // 对自由粒子的弹性和阻尼矩阵取行和界。选择 K*h²+2*C*h<=1，
    // 小于单个阻尼振子半隐式 Euler 的边界 4，为网格耦合保留余量。
    std::vector<double> stiffness_sums(count, 0), damping_sums(count, 0);
    for (const auto& edge : springs_) {
        for (const auto index : {edge.a, edge.b}) {
            stiffness_sums[index] += edge.stiffness;
            damping_sums[index] += edge.damping;
        }
    }
    double k_bound = 0, c_bound = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (!pinned_[i]) {
            k_bound = std::max(k_bound, 2 * stiffness_sums[i] / masses_[i]);
            c_bound = std::max(c_bound, 2 * damping_sums[i] / masses_[i]);
        }
    }
    const double required =
        std::max(1.0, std::ceil(parameters_.frame_seconds *
                                (std::sqrt(c_bound * c_bound + k_bound) + c_bound)));
    require(required <= 4096, "Cloth stiffness and density require more than 4096 substeps");
    minimum_substeps_ = static_cast<std::uint32_t>(required);
    require(parameters_.substeps >= minimum_substeps_,
            "Cloth requires at least " + std::to_string(minimum_substeps_) +
                " substeps; increase substeps or reduce stiffness");
    validate_anchors(environment_);
    reset();
    cancellation.check();
}

const ClothGrid& ClothSimulation::grid() const noexcept {
    return grid_;
}
const ClothParameters& ClothSimulation::parameters() const noexcept {
    return parameters_;
}
const ClothEnvironment& ClothSimulation::environment() const noexcept {
    return environment_;
}
const ClothState& ClothSimulation::state() const noexcept {
    return state_;
}
std::span<const double> ClothSimulation::masses() const noexcept {
    return masses_;
}
std::span<const ClothSpring> ClothSimulation::springs() const noexcept {
    return springs_;
}
std::span<const std::array<std::uint32_t, 3>> ClothSimulation::triangles() const noexcept {
    return triangles_;
}
std::uint32_t ClothSimulation::minimum_substeps() const noexcept {
    return minimum_substeps_;
}

void ClothSimulation::reset() {
    state_ = {rest_, std::vector<glm::dvec3>(rest_.size(), glm::dvec3(0)), 0};
}

void ClothSimulation::validate_anchors(const ClothEnvironment& environment) const {
    for (const auto index : grid_.pinned) {
        for (const auto& plane : environment.planes) {
            require(glm::dot(rest_[index] - plane.point, plane.normal) >=
                        parameters_.thickness - 1e-10,
                    "Cloth fixed point is inside a collision plane");
        }
        for (const auto& sphere : environment.spheres) {
            require(glm::length(rest_[index] - sphere.center) >=
                        sphere.radius + parameters_.thickness - 1e-10,
                    "Cloth fixed point is inside a collision sphere");
        }
    }
}

void ClothSimulation::set_environment(ClothEnvironment environment) {
    auto next = normalized_environment(std::move(environment));
    validate_anchors(next);
    environment_ = std::move(next);
}

void ClothSimulation::validate_state(ClothState& state, CancellationToken cancellation) const {
    require(state.positions.size() == rest_.size() && state.velocities.size() == rest_.size(),
            "Cloth state dimensions differ from the grid");
    for (std::size_t i = 0; i < rest_.size(); ++i) {
        if (i % 512 == 0) {
            cancellation.check();
        }
        require(finite(state.positions[i]) && finite(state.velocities[i]),
                "Cloth state contains nonfinite values");
        if (pinned_[i]) {
            require(glm::length(state.positions[i] - rest_[i]) <= 1e-10 &&
                        glm::length(state.velocities[i]) <= 1e-10,
                    "Cloth state moved a fixed point");
            state.positions[i] = rest_[i];
            state.velocities[i] = glm::dvec3(0);
        }
    }
}

void ClothSimulation::set_state(ClothState state, CancellationToken cancellation) {
    validate_state(state, cancellation);
    cancellation.check();
    state_ = std::move(state);
}

void ClothSimulation::calculate_forces(const ClothState& state, std::span<glm::dvec3> values,
                                       CancellationToken cancellation) const {
    for (std::size_t i = 0; i < rest_.size(); ++i) {
        if (i % 512 == 0) {
            cancellation.check();
        }
        values[i] = environment_.gravity * masses_[i];
    }
    for (std::size_t i = 0; i < springs_.size(); ++i) {
        if (i % 512 == 0) {
            cancellation.check();
        }
        const auto& edge = springs_[i];
        if (edge.stiffness == 0) {
            continue;
        }
        const auto force =
            cloth_spring_force(edge, state.positions[edge.a], state.positions[edge.b],
                               state.velocities[edge.a], state.velocities[edge.b]);
        values[edge.a] += force;
        values[edge.b] -= force;
    }
    const double drag = .5 * environment_.air_density * environment_.drag_coefficient / 3;
    if (drag == 0) {
        return;
    }
    for (std::size_t i = 0; i < triangles_.size(); ++i) {
        if (i % 512 == 0) {
            cancellation.check();
        }
        const auto [a, b, c] = triangles_[i];
        const auto area_normal = glm::cross(state.positions[b] - state.positions[a],
                                            state.positions[c] - state.positions[a]);
        const double twice_area = glm::length(area_normal);
        if (twice_area == 0) {
            continue;
        }
        const auto normal = area_normal / twice_area;
        const auto velocity =
            (state.velocities[a] + state.velocities[b] + state.velocities[c]) / 3.0;
        const double speed = glm::dot(environment_.wind_velocity - velocity, normal);
        const auto force = drag * (.5 * twice_area) * speed * std::abs(speed) * normal;
        values[a] += force;
        values[b] += force;
        values[c] += force;
    }
}

std::vector<glm::dvec3> ClothSimulation::forces(CancellationToken cancellation) const {
    cancellation.check();
    std::vector<glm::dvec3> result(rest_.size());
    calculate_forces(state_, result, cancellation);
    return result;
}

ClothStatistics ClothSimulation::statistics(CancellationToken cancellation) const {
    ClothStatistics result;
    result.time_seconds = static_cast<double>(state_.frame) * parameters_.frame_seconds;
    for (std::size_t i = 0; i < rest_.size(); ++i) {
        if (i % 512 == 0) {
            cancellation.check();
        }
        result.kinetic_energy +=
            .5 * masses_[i] * glm::dot(state_.velocities[i], state_.velocities[i]);
        result.momentum += masses_[i] * state_.velocities[i];
    }
    for (std::size_t i = 0; i < springs_.size(); ++i) {
        if (i % 512 == 0) {
            cancellation.check();
        }
        const auto& edge = springs_[i];
        const double length = glm::length(state_.positions[edge.b] - state_.positions[edge.a]);
        const double stretch = length - edge.rest_length;
        result.elastic_energy += .5 * edge.stiffness * stretch * stretch;
        result.maximum_spring_ratio =
            std::max(result.maximum_spring_ratio, length / edge.rest_length);
    }
    cancellation.check();
    return result;
}

ClothStepReport ClothSimulation::advance(std::uint32_t frames, CancellationToken cancellation,
                                         ClothProgress progress) {
    cancellation.check();
    require(frames <= std::numeric_limits<std::uint64_t>::max() - state_.frame &&
                std::size_t(frames) <=
                    std::numeric_limits<std::size_t>::max() / parameters_.substeps,
            "Cloth frame count overflow");
    const auto total = std::size_t(frames) * parameters_.substeps;
    const auto report = [&](std::size_t completed) {
        cancellation.check();
        if (progress) {
            progress(completed, total);
        }
        cancellation.check();
    };
    report(0);
    auto next = state_;
    std::vector<glm::dvec3> force(rest_.size());
    ClothStepReport result;
    const double h = parameters_.frame_seconds / parameters_.substeps;
    const double decay = std::exp(-parameters_.velocity_damping * h);
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        for (std::uint32_t step = 0; step < parameters_.substeps; ++step) {
            calculate_forces(next, force, cancellation);
            for (std::size_t i = 0; i < rest_.size(); ++i) {
                if (i % 512 == 0) {
                    cancellation.check();
                }
                if (pinned_[i]) {
                    continue;
                }
                next.velocities[i] = (next.velocities[i] + h * force[i] / masses_[i]) * decay;
                if (!finite(next.velocities[i])) {
                    throw std::runtime_error("Cloth integration produced a nonfinite velocity");
                }
                const auto contacts =
                    detail::move_cloth_particle(next.positions[i], next.velocities[i], h,
                                                parameters_.thickness, environment_, cancellation);
                if (!finite(next.positions[i])) {
                    throw std::runtime_error("Cloth integration produced a nonfinite position");
                }
                result.contacts += contacts.count;
                result.contact_limits += contacts.limited ? 1 : 0;
            }
            report(++result.substeps);
        }
        ++next.frame;
        validate_state(next, cancellation);
    }
    cancellation.check();
    state_ = std::move(next);
    return result;
}

} // namespace minirender
