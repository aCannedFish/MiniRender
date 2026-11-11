#include "simulation/cloth_collision.hpp"
#include <minirender/simulation/cloth.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <set>

using namespace minirender;
using Catch::Approx;

namespace {

ClothEnvironment vacuum() {
    ClothEnvironment result;
    result.gravity = glm::dvec3(0);
    result.air_density = 0;
    return result;
}
ClothParameters free_particles() {
    ClothParameters result;
    result.structural_stiffness = 0;
    result.shear_stiffness = 0;
    result.bending_stiffness = 0;
    result.velocity_damping = 0;
    result.spring_damping_ratio = 0;
    result.substeps = 1;
    return result;
}
ClothGrid square() {
    ClothGrid result;
    result.columns = 2;
    result.rows = 2;
    result.origin = {0, 1, 0};
    result.span_u = {1, 0, 0};
    result.span_v = {0, 0, -1};
    return result;
}
void same(const ClothState& a, const ClothState& b) {
    REQUIRE(a.frame == b.frame);
    REQUIRE(a.positions == b.positions);
    REQUIRE(a.velocities == b.velocities);
}

// 2×2 网格仅启用剪切弹簧，两个对角线独立；其中一条作无阻尼解析振子。
ClothSimulation oscillator(std::uint32_t substeps, double damping = 0) {
    auto parameters = free_particles();
    parameters.surface_density = 1;
    parameters.shear_stiffness = 12;
    parameters.spring_damping_ratio = damping;
    parameters.substeps = substeps;
    ClothSimulation simulation(square(), parameters, vacuum());
    auto state = simulation.state();
    const auto direction = glm::normalize(state.positions[3] - state.positions[0]);
    state.positions[0] -= .05 * direction;
    state.positions[3] += .05 * direction;
    simulation.set_state(std::move(state));
    return simulation;
}

} // namespace

TEST_CASE("Cloth grid preserves area mass and unique spring connectivity", "[cloth]") {
    ClothGrid grid;
    grid.columns = 5;
    grid.rows = 4;
    grid.origin = {2, 3, 4};
    grid.span_u = {2, 0, 0};
    grid.span_v = {0, 0, -3};
    ClothSimulation simulation(grid, {}, vacuum());
    REQUIRE(simulation.state().positions.size() == 20);
    REQUIRE(simulation.triangles().size() == 24);
    REQUIRE(std::accumulate(simulation.masses().begin(), simulation.masses().end(), 0.0) ==
            Approx(.9));
    std::array<std::size_t, 3> counts{};
    std::set<std::pair<std::uint32_t, std::uint32_t>> edges;
    for (const auto& spring : simulation.springs()) {
        ++counts[std::size_t(spring.kind)];
        REQUIRE(edges.emplace(std::min(spring.a, spring.b), std::max(spring.a, spring.b)).second);
        REQUIRE(spring.rest_length == Approx(glm::length(simulation.state().positions[spring.b] -
                                                         simulation.state().positions[spring.a])));
        REQUIRE(glm::length(spring.rest_direction) == Approx(1));
    }
    REQUIRE(counts == std::array<std::size_t, 3>{31, 24, 22});
    for (const auto force : simulation.forces()) {
        REQUIRE(glm::length(force) < 1e-12);
    }
    REQUIRE(simulation.minimum_substeps() <= simulation.parameters().substeps);
    const auto before = simulation.state();
    simulation.advance(3);
    REQUIRE(simulation.state().positions == before.positions);
    REQUIRE(simulation.statistics().elastic_energy == 0);
}

TEST_CASE("Cloth spring force has analytic stiffness damping and zero length recovery", "[cloth]") {
    const ClothSpring spring{0, 1, ClothSpringKind::structural, 1, 8, .5, {1, 0, 0}};
    const auto force = cloth_spring_force(spring, {0, 0, 0}, {1.2, 0, 0}, {0, 0, 0}, {.5, 0, 0});
    REQUIRE(force.x == Approx(1.85));
    REQUIRE(force.y == 0);
    REQUIRE(force.z == 0);
    const auto reversed = cloth_spring_force(spring, {1.2, 0, 0}, {0, 0, 0}, {.5, 0, 0}, {0, 0, 0});
    REQUIRE(glm::length(force + reversed) < 1e-14);
    const auto damping = cloth_spring_force(spring, {0, 0, 0}, {1, 0, 0}, {0, 0, 0}, {.5, 0, 0});
    REQUIRE(glm::dot(damping, glm::dvec3(-.5, 0, 0)) == Approx(-.125));
    REQUIRE(cloth_spring_force(spring, {}, {}, {}, {}) == glm::dvec3(-8, 0, 0));
}

TEST_CASE("Cloth free fall follows semi implicit Euler and converges with fixed substeps",
          "[cloth]") {
    auto parameters = free_particles();
    auto environment = vacuum();
    environment.gravity = {0, -9.81, 0};
    double previous_error = 1;
    for (const auto substeps : {4U, 8U, 16U}) {
        parameters.substeps = substeps;
        ClothSimulation simulation(square(), parameters, environment);
        const auto initial = simulation.state();
        simulation.advance(60);
        const double h = parameters.frame_seconds / substeps;
        const double displacement = -.5 * 9.81 * (1 + h);
        for (std::size_t i = 0; i < initial.positions.size(); ++i) {
            REQUIRE(simulation.state().positions[i].y ==
                    Approx(initial.positions[i].y + displacement).margin(1e-11));
            REQUIRE(simulation.state().velocities[i].y == Approx(-9.81).margin(1e-11));
        }
        const double error =
            std::abs(simulation.state().positions[0].y - initial.positions[0].y + .5 * 9.81);
        REQUIRE(error < previous_error * .51);
        previous_error = error;
    }
}

TEST_CASE("Cloth diagonal oscillator matches closed form and bounds energy", "[cloth]") {
    double previous_error = 1;
    for (const auto substeps : {8U, 16U, 32U}) {
        auto simulation = oscillator(substeps);
        const double omega = std::sqrt(24 / simulation.masses()[0]);
        simulation.advance(30);
        const double extension =
            glm::length(simulation.state().positions[3] - simulation.state().positions[0]) -
            std::sqrt(2.0);
        const double error = std::abs(extension - .1 * std::cos(omega * .5));
        REQUIRE(error < .001);
        REQUIRE(error < previous_error * .6);
        previous_error = error;
    }
    auto simulation = oscillator(8);
    const double initial_energy = simulation.statistics().elastic_energy;
    for (int frame = 0; frame < 180; ++frame) {
        simulation.advance();
        const auto stats = simulation.statistics();
        REQUIRE(stats.kinetic_energy + stats.elastic_energy < 1.02 * initial_energy);
        REQUIRE(stats.kinetic_energy + stats.elastic_energy > .98 * initial_energy);
        REQUIRE(glm::length(stats.momentum) < 1e-12);
    }
    auto damped = oscillator(16, .2);
    damped.advance(120);
    const auto stats = damped.statistics();
    REQUIRE(stats.kinetic_energy + stats.elastic_energy < .01 * initial_energy);
}

TEST_CASE("Cloth fixed frames reproduce exact states and preserve anchors", "[cloth]") {
    ClothGrid grid;
    grid.columns = 9;
    grid.rows = 9;
    grid.origin = {-1, 2, 0};
    grid.span_v = {0, -1.4F, 0};
    grid.pinned = {0, 8};
    ClothEnvironment environment;
    environment.wind_velocity = {0, 0, 2};
    ClothSimulation batch(grid, {}, environment);
    ClothSimulation individual(grid, {}, environment);
    const auto initial = batch.state();
    const auto report = batch.advance(40);
    for (int frame = 0; frame < 40; ++frame) {
        individual.advance();
    }
    same(batch.state(), individual.state());
    REQUIRE(report.substeps == 40 * 64);
    REQUIRE(report.contact_limits == 0);
    for (const auto index : grid.pinned) {
        REQUIRE(batch.state().positions[index] == initial.positions[index]);
        REQUIRE(batch.state().velocities[index] == glm::dvec3(0));
    }
    REQUIRE(batch.state().positions[40].z > .01);
    auto resumed = ClothSimulation(grid, {}, environment);
    resumed.set_state(batch.state());
    resumed.advance(10);
    batch.advance(10);
    same(resumed.state(), batch.state());
    individual.reset();
    same(initial, individual.state());
}

TEST_CASE("Cloth pressure respects area winding and relative wind velocity", "[cloth]") {
    auto grid = square();
    grid.span_u = {2, 0, 0};
    grid.span_v = {0, 0, -2};
    auto environment = vacuum();
    environment.air_density = 2;
    environment.drag_coefficient = 1;
    environment.wind_velocity = {0, 3, 0};
    ClothSimulation simulation(grid, free_particles(), environment);
    const auto forces = simulation.forces();
    glm::dvec3 total(0);
    for (std::size_t i = 0; i < forces.size(); ++i) {
        REQUIRE(glm::length(forces[i] / simulation.masses()[i] - glm::dvec3(0, 60, 0)) < 1e-12);
        total += forces[i];
    }
    REQUIRE(glm::length(total - glm::dvec3(0, 36, 0)) < 1e-12);
    environment.wind_velocity.y = -3;
    simulation.set_environment(environment);
    const auto opposite = simulation.forces();
    for (std::size_t i = 0; i < forces.size(); ++i) {
        REQUIRE(opposite[i] == -forces[i]);
    }
    auto state = simulation.state();
    std::fill(state.velocities.begin(), state.velocities.end(), environment.wind_velocity);
    simulation.set_state(state);
    for (const auto force : simulation.forces()) {
        REQUIRE(force == glm::dvec3(0));
    }
    grid.span_v.z = 2;
    ClothSimulation reversed(grid, free_particles(), environment);
    total = glm::dvec3(0);
    for (const auto force : reversed.forces()) {
        total += force;
    }
    REQUIRE(glm::length(total - glm::dvec3(0, -36, 0)) < 1e-12);
}

TEST_CASE("Cloth swept plane collision preserves sliding and applies Coulomb friction",
          "[cloth][collision]") {
    auto environment = vacuum();
    environment.planes.push_back({{}, {0, 1, 0}, 0});
    glm::dvec3 position(0, 1, 0), velocity(2, -10, 0);
    auto contacts = detail::move_cloth_particle(position, velocity, .2, .01, environment, {});
    REQUIRE(position.x == Approx(.4));
    REQUIRE(position.y >= .01);
    REQUIRE(velocity == glm::dvec3(2, 0, 0));
    REQUIRE(contacts.count == 1);
    REQUIRE_FALSE(contacts.limited);
    environment.planes[0].friction = .1;
    position = {0, 1, 0};
    velocity = {2, -10, 0};
    detail::move_cloth_particle(position, velocity, .2, .01, environment, {});
    REQUIRE(velocity.x == Approx(1));
    REQUIRE(position.x == Approx(.299));
    REQUIRE(velocity.y == 0);
}

TEST_CASE("Cloth swept sphere catches traversals with both endpoints outside",
          "[cloth][collision]") {
    auto environment = vacuum();
    environment.spheres.push_back({{}, .5, 0});
    glm::dvec3 position(-2, 0, 0), velocity(40, 0, 0);
    const auto contacts = detail::move_cloth_particle(position, velocity, .1, .01, environment, {});
    REQUIRE(contacts.count == 1);
    REQUIRE(position.x <= -.51);
    REQUIRE(velocity == glm::dvec3(0));
    position = {-2, .3, 0};
    velocity = {40, 0, 0};
    detail::move_cloth_particle(position, velocity, .1, .01, environment, {});
    REQUIRE(glm::length(position) >= .51);
    REQUIRE(velocity.y > 0);
    position = {};
    velocity = {};
    detail::move_cloth_particle(position, velocity, .1, .01, environment, {});
    REQUIRE(position.y >= .51);
    auto grid = square();
    grid.origin = {-2, 1, 0};
    auto parameters = free_particles();
    parameters.frame_seconds = .1;
    parameters.thickness = .01;
    environment.spheres[0].center = {0, 1, 0};
    ClothSimulation simulation(grid, parameters, environment);
    auto state = simulation.state();
    std::fill(state.velocities.begin(), state.velocities.end(), glm::dvec3(40, 0, 0));
    simulation.set_state(state);
    simulation.advance();
    REQUIRE(simulation.state().positions[0].x <= -.51);
    REQUIRE(simulation.state().velocities[0] == glm::dvec3(0));
    REQUIRE(simulation.state().positions[3].x == Approx(3));
}

TEST_CASE("Cloth collision events handle corners and reject failed projection",
          "[cloth][collision]") {
    auto environment = vacuum();
    environment.planes = {{{}, {1, 0, 0}, 0}, {{}, {0, 1, 0}, 0}};
    glm::dvec3 position(1, 1, 0), velocity(-10, -20, 3);
    const auto contacts = detail::move_cloth_particle(position, velocity, .1, .01, environment, {});
    REQUIRE(contacts.count == 2);
    REQUIRE_FALSE(contacts.limited);
    REQUIRE(position.x >= .01);
    REQUIRE(position.y >= .01);
    REQUIRE(position.z == Approx(.3));
    REQUIRE(velocity == glm::dvec3(0, 0, 3));
    environment.planes = {{{1, 0, 0}, {1, 0, 0}, 0}, {{}, {-1, 0, 0}, 0}};
    REQUIRE_THROWS_AS(detail::move_cloth_particle(position, velocity, .1, .01, environment, {}),
                      std::runtime_error);
    ClothSimulation simulation(square(), free_particles(), environment);
    const auto before = simulation.state();
    REQUIRE_THROWS_AS(simulation.advance(), std::runtime_error);
    same(before, simulation.state());
}

TEST_CASE("Cloth cancellation and callback errors leave the previous state intact", "[cloth]") {
    auto simulation = ClothSimulation();
    const auto before = simulation.state();
    CancellationSource source;
    std::size_t completed = 0;
    REQUIRE_THROWS_AS(simulation.advance(100, source.token(),
                                         [&](std::size_t value, std::size_t total) {
                                             REQUIRE(total == 6400);
                                             REQUIRE(value >= completed);
                                             completed = value;
                                             if (value == 7) {
                                                 source.request();
                                             }
                                         }),
                      OperationCancelled);
    REQUIRE(completed == 7);
    same(before, simulation.state());
    REQUIRE_THROWS_AS(simulation.advance(2, {},
                                         [](std::size_t value, std::size_t) {
                                             if (value == 3) {
                                                 throw std::runtime_error("callback");
                                             }
                                         }),
                      std::runtime_error);
    same(before, simulation.state());
    REQUIRE_THROWS_AS(simulation.freeze(source.token()), OperationCancelled);
    REQUIRE_THROWS_AS(simulation.forces(source.token()), OperationCancelled);
    REQUIRE_THROWS_AS(simulation.statistics(source.token()), OperationCancelled);
    REQUIRE_THROWS_AS(ClothSimulation({}, {}, {}, source.token()), OperationCancelled);
}

TEST_CASE("Cloth freeze keeps local details UV topology and immutable geometry", "[cloth]") {
    auto grid = square();
    grid.origin = {1e6F, -1e6F, 1e6F};
    grid.columns = 4;
    grid.rows = 5;
    grid.span_u = {.01F, 0, 0};
    grid.span_v = {0, 0, -.02F};
    ClothSimulation simulation(grid, free_particles(), vacuum());
    const auto frozen = simulation.freeze();
    frozen.mesh->validate();
    REQUIRE(frozen.transform.translation == grid.origin);
    REQUIRE(frozen.mesh->positions.size() == 20);
    REQUIRE(frozen.mesh->triangles.size() == 24);
    for (std::size_t i = 0; i < frozen.mesh->positions.size(); ++i) {
        const auto world =
            glm::dvec3(frozen.mesh->positions[i]) + glm::dvec3(frozen.transform.translation);
        REQUIRE(glm::length(world - simulation.state().positions[i]) < 1e-9);
    }
    for (const auto& triangle : frozen.mesh->triangles) {
        for (const auto& corner : triangle.corners) {
            REQUIRE(glm::length(corner.normal - glm::vec3(0, 1, 0)) < 1e-6F);
            REQUIRE(corner.uv.x ==
                    Approx(double(corner.position % grid.columns) / (grid.columns - 1)));
            REQUIRE(corner.uv.y ==
                    Approx(double(corner.position / grid.columns) / (grid.rows - 1)));
        }
    }
    const auto positions = frozen.mesh->positions;
    auto environment = vacuum();
    environment.gravity.y = -1;
    simulation.set_environment(environment);
    simulation.advance();
    REQUIRE(frozen.mesh->positions == positions);
    REQUIRE(simulation.state().positions[0].y < grid.origin.y);
}

TEST_CASE("Cloth freeze resolves cancelling crease normals and rejects degenerate faces",
          "[cloth]") {
    ClothGrid grid;
    grid.columns = 3;
    grid.rows = 3;
    grid.origin = {};
    grid.span_u = {2, 0, 0};
    grid.span_v = {0, 0, -2};
    ClothSimulation simulation(grid, free_particles(), vacuum());
    auto state = simulation.state();
    for (std::size_t i = 6; i < 9; ++i) {
        state.positions[i] = state.positions[i - 6];
    }
    simulation.set_state(state);
    const auto folded = simulation.freeze();
    folded.mesh->validate();
    for (const auto& face : folded.mesh->triangles) {
        for (const auto& corner : face.corners) {
            REQUIRE(glm::length(corner.normal) == Approx(1));
        }
    }
    state.positions[1] = state.positions[0];
    simulation.set_state(state);
    REQUIRE_THROWS_AS(simulation.freeze(), std::runtime_error);
}

TEST_CASE("Cloth validation rejects unstable steps invalid data and obstructed anchors",
          "[cloth]") {
    auto grid = square();
    grid.pinned = {0, 0};
    REQUIRE_THROWS_AS(ClothSimulation(grid), std::invalid_argument);
    grid = square();
    grid.span_v = grid.span_u;
    REQUIRE_THROWS_AS(ClothSimulation(grid), std::invalid_argument);
    auto parameters = ClothParameters();
    parameters.substeps = 1;
    REQUIRE_THROWS_AS(ClothSimulation({}, parameters), std::invalid_argument);
    parameters.surface_density = -1;
    REQUIRE_THROWS_AS(ClothSimulation({}, parameters), std::invalid_argument);
    grid = square();
    grid.pinned = {0};
    auto simulation = ClothSimulation(grid, {}, vacuum());
    const auto before = simulation.state();
    auto state = before;
    state.positions[0].x += .1;
    REQUIRE_THROWS_AS(simulation.set_state(state), std::invalid_argument);
    state = before;
    state.velocities[1].z = std::numeric_limits<double>::infinity();
    REQUIRE_THROWS_AS(simulation.set_state(state), std::invalid_argument);
    same(before, simulation.state());
    auto environment = vacuum();
    environment.spheres.push_back({before.positions[0], .5, .3});
    REQUIRE_THROWS_AS(simulation.set_environment(environment), std::invalid_argument);
    REQUIRE(simulation.environment().spheres.empty());
    environment = vacuum();
    environment.planes.push_back({{}, {0, 2, 0}, .3});
    simulation.set_environment(environment);
    REQUIRE(simulation.environment().planes[0].normal == glm::dvec3(0, 1, 0));
    environment.planes[0].normal = {1e308, 0, 0};
    REQUIRE_THROWS_AS(simulation.set_environment(environment), std::invalid_argument);
}

TEST_CASE("Cloth drapes over a sphere while staying outside collision solids",
          "[cloth][simulation]") {
    ClothEnvironment environment;
    environment.planes.push_back({});
    environment.spheres.push_back({});
    ClothSimulation simulation({}, {}, environment);
    const auto report = simulation.advance(90);
    REQUIRE(report.contacts > 100);
    REQUIRE(report.contact_limits == 0);
    const auto& state = simulation.state();
    for (const auto point : state.positions) {
        REQUIRE(point.y >= simulation.parameters().thickness - 1e-9);
        REQUIRE(glm::length(point - environment.spheres[0].center) >=
                .65 + simulation.parameters().thickness - 1e-9);
    }
    REQUIRE(state.positions[0].y < 1);
    const auto center = std::size_t(12 * 25 + 12);
    REQUIRE(state.positions[center].y == Approx(1.306).margin(.02));
    REQUIRE(simulation.statistics().maximum_spring_ratio < 1.5);
    simulation.freeze().mesh->validate();
}
