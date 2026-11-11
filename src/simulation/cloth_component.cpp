#include <minirender/simulation/cloth_component.hpp>

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace minirender {
namespace {

bool finite(glm::dvec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::invalid_argument(message);
    }
}
glm::dvec3 rest_point(const ClothGrid& grid, std::uint32_t index) {
    return grid.origin + grid.span_u * (double(index % grid.columns) / (grid.columns - 1)) +
           grid.span_v * (double(index / grid.columns) / (grid.rows - 1));
}

} // namespace

void ClothCollider::validate() const {
    require(kind == ClothColliderKind::sphere || kind == ClothColliderKind::plane,
            "Unknown cloth collider kind");
    require(finite(point) && finite(normal) && std::isfinite(glm::length(normal)) &&
                glm::length(normal) > 1e-12 && std::isfinite(radius) && radius > 0 &&
                radius <= 1e6 && std::isfinite(friction) && friction >= 0 && friction <= 1,
            "Invalid cloth collider values");
}
void ClothDefinition::validate() const {
    grid.validate();
    parameters.validate();
    environment.validate();
}

ClothComponent::ClothComponent(std::shared_ptr<const ClothDefinition> definition,
                               ClothState local_state, std::shared_ptr<const MeshData> mesh,
                               CancellationToken cancellation)
    : definition_(std::move(definition)), state_(std::move(local_state)), mesh_(std::move(mesh)) {
    cancellation.check();
    require(definition_ != nullptr && mesh_ != nullptr, "Cloth component data is missing");
    definition_->validate();
    const auto& grid = definition_->grid;
    const auto count = std::size_t(grid.columns) * grid.rows;
    require(state_.positions.size() == count && state_.velocities.size() == count &&
                mesh_->positions.size() == count &&
                mesh_->triangles.size() == 2 * std::size_t(grid.columns - 1) * (grid.rows - 1),
            "Cloth component dimensions differ from the mesh");
    mesh_->validate(cancellation);
    for (std::size_t i = 0; i < count; ++i) {
        if (i % 512 == 0) {
            cancellation.check();
        }
        require(finite(state_.positions[i]) && finite(state_.velocities[i]),
                "Cloth component has nonfinite state");
        require(glm::vec3(state_.positions[i]) == mesh_->positions[i],
                "Cloth component state differs from its mesh");
    }
    for (const auto index : grid.pinned) {
        require(glm::length(state_.positions[index] - rest_point(grid, index)) < 1e-9 &&
                    glm::length(state_.velocities[index]) < 1e-9,
                "Cloth component moved a fixed point");
    }
    for (std::size_t face = 0; face < mesh_->triangles.size(); ++face) {
        if (face % 512 == 0) {
            cancellation.check();
        }
        const auto cell = face / 2;
        const auto a = std::uint32_t(cell / (grid.columns - 1)) * grid.columns +
                       std::uint32_t(cell % (grid.columns - 1));
        const auto b = a + grid.columns;
        const auto indices = face % 2 == 0 ? std::array{a, a + 1, b + 1} : std::array{a, b + 1, b};
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const auto& value = mesh_->triangles[face].corners[corner];
            const auto index = indices[corner];
            require(value.position == index, "Freeze cloth before changing its topology");
            const glm::vec2 uv{float(index % grid.columns) / float(grid.columns - 1),
                               float(index / grid.columns) / float(grid.rows - 1)};
            require(glm::length(value.uv - uv) < 1e-6F, "Cloth component UV coordinates changed");
        }
    }
}
const std::shared_ptr<const ClothDefinition>& ClothComponent::definition() const noexcept {
    return definition_;
}
const ClothState& ClothComponent::state() const noexcept {
    return state_;
}
const std::shared_ptr<const MeshData>& ClothComponent::mesh() const noexcept {
    return mesh_;
}

ClothEnvironment cloth_environment(const ClothDefinition& definition, const SceneDocument& scene) {
    auto result = definition.environment;
    if (!definition.use_scene_colliders) {
        return result;
    }
    for (const auto& object : scene.objects) {
        if (!object.cloth_collider || !object.cloth_collider->enabled) {
            continue;
        }
        const auto& collider = *object.cloth_collider;
        const glm::dmat4 matrix(object.transform.matrix());
        const auto point = glm::dvec3(matrix * glm::dvec4(collider.point, 1));
        if (collider.kind == ClothColliderKind::sphere) {
            const double x = glm::length(glm::dvec3(matrix[0]));
            const double y = glm::length(glm::dvec3(matrix[1]));
            const double z = glm::length(glm::dvec3(matrix[2]));
            require(std::abs(x - y) <= 1e-5 * x && std::abs(x - z) <= 1e-5 * x,
                    "Sphere cloth collider requires uniform scale");
            result.spheres.push_back({point, collider.radius * x, collider.friction});
        } else {
            const auto normal =
                glm::normalize(glm::transpose(glm::inverse(glm::dmat3(matrix))) * collider.normal);
            result.planes.push_back({point, normal, collider.friction});
        }
    }
    result.validate();
    return result;
}

std::unique_ptr<ClothSimulation> make_cloth_simulation(const ClothComponent& component,
                                                       const Transform& transform,
                                                       ClothEnvironment environment,
                                                       CancellationToken cancellation) {
    const auto& definition = *component.definition();
    auto grid = definition.grid;
    const glm::dmat4 matrix(transform.matrix());
    grid.origin = glm::dvec3(matrix * glm::dvec4(grid.origin, 1));
    grid.span_u = glm::dmat3(matrix) * grid.span_u;
    grid.span_v = glm::dmat3(matrix) * grid.span_v;
    auto parameters = definition.parameters;
    parameters.substeps = 4096;
    auto simulation =
        std::make_unique<ClothSimulation>(grid, parameters, environment, cancellation);
    const auto actual = std::max(definition.parameters.substeps, simulation->minimum_substeps());
    if (actual != parameters.substeps) {
        parameters.substeps = actual;
        simulation = std::make_unique<ClothSimulation>(grid, parameters, environment, cancellation);
    }
    auto state = component.state();
    for (std::size_t i = 0; i < state.positions.size(); ++i) {
        if (i % 512 == 0) {
            cancellation.check();
        }
        state.positions[i] = glm::dvec3(matrix * glm::dvec4(state.positions[i], 1));
        state.velocities[i] = glm::dmat3(matrix) * state.velocities[i];
    }
    for (const auto index : grid.pinned) {
        state.positions[index] = simulation->state().positions[index];
        state.velocities[index] = glm::dvec3(0);
    }
    simulation->set_state(std::move(state), cancellation);
    return simulation;
}

ClothPreparedFrame prepare_cloth_frame(const ClothSimulation& simulation,
                                       std::shared_ptr<const ClothDefinition> definition,
                                       const Transform& transform, CancellationToken cancellation) {
    const auto frozen = simulation.freeze(cancellation);
    auto state = simulation.state();
    const glm::dmat4 matrix(transform.matrix());
    const auto inverse = glm::inverse(matrix);
    for (std::size_t i = 0; i < state.positions.size(); ++i) {
        if (i % 512 == 0) {
            cancellation.check();
        }
        state.positions[i] = glm::dvec3(inverse * glm::dvec4(state.positions[i], 1));
        state.velocities[i] = glm::dmat3(inverse) * state.velocities[i];
    }
    for (const auto index : definition->grid.pinned) {
        state.positions[index] = rest_point(definition->grid, index);
        state.velocities[index] = glm::dvec3(0);
    }
    auto mesh = std::make_shared<MeshData>(*frozen.mesh);
    for (std::size_t i = 0; i < state.positions.size(); ++i) {
        mesh->positions[i] = glm::vec3(state.positions[i]);
    }
    const auto normal_to_local = glm::transpose(glm::dmat3(matrix));
    const double sign = glm::determinant(glm::dmat3(matrix)) < 0 ? -1 : 1;
    for (std::size_t i = 0; i < mesh->triangles.size(); ++i) {
        if (i % 512 == 0) {
            cancellation.check();
        }
        for (auto& corner : mesh->triangles[i].corners) {
            corner.normal =
                glm::vec3(sign * glm::normalize(normal_to_local * glm::dvec3(corner.normal)));
        }
    }
    auto component = std::make_shared<const ClothComponent>(std::move(definition), std::move(state),
                                                            mesh, cancellation);
    return {component, simulation.statistics(cancellation), simulation.parameters().substeps};
}

std::shared_ptr<const ClothComponent> make_cloth_component(ClothDefinition definition,
                                                           CancellationToken cancellation) {
    definition.validate();
    auto parameters = definition.parameters;
    parameters.substeps = 4096;
    ClothEnvironment empty;
    empty.gravity = glm::dvec3(0);
    empty.air_density = 0;
    ClothSimulation simulation(definition.grid, parameters, empty, cancellation);
    const auto data = std::make_shared<const ClothDefinition>(std::move(definition));
    return prepare_cloth_frame(simulation, data, {}, cancellation).component;
}

} // namespace minirender
