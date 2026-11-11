#include "cloth_simulation_io.hpp"

#include <limits>
#include <stdexcept>

namespace minirender::detail {
namespace {
using Json = nlohmann::json;

glm::dvec3 vector(const Json& value) {
    if (!value.is_array() || value.size() != 3) {
        throw std::invalid_argument("Cloth vector requires three numbers");
    }
    return {value.at(0).get<double>(), value.at(1).get<double>(), value.at(2).get<double>()};
}
Json array(glm::dvec3 value) {
    return Json::array({value.x, value.y, value.z});
}
std::uint64_t integer(const Json& value) {
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>();
    }
    if (value.is_number_integer() && value.get<std::int64_t>() >= 0) {
        return std::uint64_t(value.get<std::int64_t>());
    }
    throw std::invalid_argument("Cloth count must be a nonnegative integer");
}
std::uint32_t count(const Json& value, std::uint32_t low, std::uint32_t high) {
    const auto result = integer(value);
    if (result < low || result > high) {
        throw std::invalid_argument("Cloth count is outside its supported range");
    }
    return static_cast<std::uint32_t>(result);
}
} // namespace

std::shared_ptr<const ClothComponent> read_cloth_component(const Json& value,
                                                           std::shared_ptr<const MeshData> mesh) {
    ClothDefinition definition;
    const auto& grid = value.at("grid");
    definition.grid.columns = count(grid.at("columns"), 2, 256);
    definition.grid.rows = count(grid.at("rows"), 2, 256);
    definition.grid.origin = vector(grid.at("origin"));
    definition.grid.span_u = vector(grid.at("span_u"));
    definition.grid.span_v = vector(grid.at("span_v"));
    const auto size = definition.grid.columns * definition.grid.rows;
    if (!grid.at("pinned").is_array() || grid.at("pinned").size() > size) {
        throw std::invalid_argument("Invalid cloth fixed point list");
    }
    for (const auto& index : grid.at("pinned")) {
        definition.grid.pinned.push_back(count(index, 0, size - 1));
    }
    const auto& parameters = value.at("parameters");
    auto& p = definition.parameters;
    p.surface_density = parameters.at("surface_density").get<double>();
    p.structural_stiffness = parameters.at("structural_stiffness").get<double>();
    p.shear_stiffness = parameters.at("shear_stiffness").get<double>();
    p.bending_stiffness = parameters.at("bending_stiffness").get<double>();
    p.spring_damping_ratio = parameters.at("spring_damping_ratio").get<double>();
    p.velocity_damping = parameters.at("velocity_damping").get<double>();
    p.thickness = parameters.at("thickness").get<double>();
    p.frame_seconds = parameters.at("frame_seconds").get<double>();
    p.substeps = count(parameters.at("substeps"), 1, 4096);
    const auto& environment = value.at("environment");
    auto& e = definition.environment;
    e.gravity = vector(environment.at("gravity"));
    e.wind_velocity = vector(environment.at("wind_velocity"));
    e.air_density = environment.at("air_density").get<double>();
    e.drag_coefficient = environment.at("drag_coefficient").get<double>();
    if (!environment.at("planes").is_array() || environment.at("planes").size() > 32 ||
        !environment.at("spheres").is_array() || environment.at("spheres").size() > 32) {
        throw std::invalid_argument("Too many cloth collision objects");
    }
    for (const auto& plane : environment.at("planes")) {
        e.planes.push_back({vector(plane.at("point")), vector(plane.at("normal")),
                            plane.at("friction").get<double>()});
    }
    for (const auto& sphere : environment.at("spheres")) {
        e.spheres.push_back({vector(sphere.at("center")), sphere.at("radius").get<double>(),
                             sphere.at("friction").get<double>()});
    }
    definition.use_scene_colliders = value.at("use_scene_colliders").get<bool>();
    const auto& positions = value.at("positions");
    const auto& velocities = value.at("velocities");
    if (!positions.is_array() || !velocities.is_array() || positions.size() != size ||
        velocities.size() != size) {
        throw std::invalid_argument("Cloth saved state dimensions differ from its grid");
    }
    ClothState state;
    state.frame = integer(value.at("frame"));
    state.positions.reserve(size);
    state.velocities.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        state.positions.push_back(vector(positions.at(i)));
        state.velocities.push_back(vector(velocities.at(i)));
    }
    return std::make_shared<const ClothComponent>(
        std::make_shared<const ClothDefinition>(std::move(definition)), std::move(state),
        std::move(mesh));
}

Json write_cloth_component(const ClothComponent& component) {
    const auto& definition = *component.definition();
    const auto& g = definition.grid;
    const auto& p = definition.parameters;
    const auto& e = definition.environment;
    Json environment{{"gravity", array(e.gravity)},  {"wind_velocity", array(e.wind_velocity)},
                     {"air_density", e.air_density}, {"drag_coefficient", e.drag_coefficient},
                     {"planes", Json::array()},      {"spheres", Json::array()}};
    for (const auto& plane : e.planes) {
        environment["planes"].push_back({{"point", array(plane.point)},
                                         {"normal", array(plane.normal)},
                                         {"friction", plane.friction}});
    }
    for (const auto& sphere : e.spheres) {
        environment["spheres"].push_back({{"center", array(sphere.center)},
                                          {"radius", sphere.radius},
                                          {"friction", sphere.friction}});
    }
    Json result{{"grid",
                 {{"columns", g.columns},
                  {"rows", g.rows},
                  {"origin", array(g.origin)},
                  {"span_u", array(g.span_u)},
                  {"span_v", array(g.span_v)},
                  {"pinned", g.pinned}}},
                {"parameters",
                 {{"surface_density", p.surface_density},
                  {"structural_stiffness", p.structural_stiffness},
                  {"shear_stiffness", p.shear_stiffness},
                  {"bending_stiffness", p.bending_stiffness},
                  {"spring_damping_ratio", p.spring_damping_ratio},
                  {"velocity_damping", p.velocity_damping},
                  {"thickness", p.thickness},
                  {"frame_seconds", p.frame_seconds},
                  {"substeps", p.substeps}}},
                {"environment", std::move(environment)},
                {"use_scene_colliders", definition.use_scene_colliders},
                {"frame", component.state().frame},
                {"positions", Json::array()},
                {"velocities", Json::array()}};
    for (const auto position : component.state().positions) {
        result["positions"].push_back(array(position));
    }
    for (const auto velocity : component.state().velocities) {
        result["velocities"].push_back(array(velocity));
    }
    return result;
}

ClothCollider read_cloth_collider(const Json& value) {
    ClothCollider result;
    const auto kind = value.at("kind").get<std::string>();
    if (kind == "sphere") {
        result.kind = ClothColliderKind::sphere;
    } else if (kind == "plane") {
        result.kind = ClothColliderKind::plane;
    } else {
        throw std::invalid_argument("Unknown cloth collider kind");
    }
    result.point = vector(value.at("point"));
    result.normal = vector(value.at("normal"));
    result.radius = value.at("radius").get<double>();
    result.friction = value.at("friction").get<double>();
    result.enabled = value.at("enabled").get<bool>();
    result.validate();
    return result;
}
Json write_cloth_collider(const ClothCollider& collider) {
    return {{"kind", collider.kind == ClothColliderKind::sphere ? "sphere" : "plane"},
            {"point", array(collider.point)},
            {"normal", array(collider.normal)},
            {"radius", collider.radius},
            {"friction", collider.friction},
            {"enabled", collider.enabled}};
}

} // namespace minirender::detail
