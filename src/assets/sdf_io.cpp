#include "staged_file.hpp"
#include <minirender/assets/sdf_io.hpp>

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace minirender {
namespace {
using Json = nlohmann::json;
glm::vec3 vector(const Json& value) {
    if (!value.is_array() || value.size() != 3) {
        throw std::invalid_argument("SDF vector requires three numbers");
    }
    return {value.at(0).get<float>(), value.at(1).get<float>(), value.at(2).get<float>()};
}
Json array(glm::vec3 value) {
    return Json::array({value.x, value.y, value.z});
}
std::uint32_t step_count(const Json& value) {
    if (!value.is_number_integer() || value < 1 || value > 4096) {
        throw std::invalid_argument("SDF step count must be an integer in 1..4096");
    }
    return value.get<std::uint32_t>();
}
} // namespace

SdfDocument load_sdf(const std::filesystem::path& path, CancellationToken cancellation) {
    cancellation.check();
    if (std::filesystem::file_size(path) > 1024 * 1024) {
        throw std::length_error("SDF file exceeds one MiB");
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot open SDF document");
    }
    Json json;
    stream >> json;
    if (json.at("format") != "minirender-sdf" || !json.at("version").is_number_integer() ||
        json.at("version") != 1 || !json.at("nodes").is_array() || json.at("nodes").empty() ||
        json.at("nodes").size() > 64) {
        throw std::invalid_argument("Invalid SDF document format");
    }
    SdfDocument document;
    std::unordered_map<std::string, std::uint32_t> ids;
    for (const auto& value : json.at("nodes")) {
        cancellation.check();
        SdfNode node;
        node.id = value.at("id").get<std::string>();
        node.name = value.value("name", node.id);
        node.kind = parse_sdf_kind(value.at("kind").get<std::string>());
        node.parameters = vector(value.value("parameters", Json::array({.7, .7, .7})));
        node.translation = vector(value.value("translation", Json::array({0, 0, 0})));
        node.rotation_degrees = vector(value.value("rotation", Json::array({0, 0, 0})));
        node.scale = value.value("scale", 1.0F);
        node.color = vector(value.value("color", Json::array({.6, .7, .8})));
        if (!ids.emplace(node.id, static_cast<std::uint32_t>(document.nodes.size())).second) {
            throw std::invalid_argument("Duplicate SDF node ID");
        }
        document.nodes.push_back(std::move(node));
    }
    const auto node_index = [&](const Json& value) {
        const auto found = ids.find(value.get<std::string>());
        if (found == ids.end()) {
            throw std::invalid_argument("SDF node reference is missing");
        }
        return found->second;
    };
    document.root = node_index(json.at("root"));
    for (std::size_t i = 0; i < document.nodes.size(); ++i) {
        auto& node = document.nodes[i];
        const auto& value = json.at("nodes").at(i);
        if (value.contains("left")) {
            node.left = node_index(value.at("left"));
        }
        if (value.contains("right")) {
            node.right = node_index(value.at("right"));
        }
    }
    const auto& camera = json.at("camera");
    document.camera.position = vector(camera.at("position"));
    document.camera.target = vector(camera.at("target"));
    document.camera.up = vector(camera.at("up"));
    document.camera.vertical_fov_degrees = camera.at("vertical_fov").get<float>();
    document.camera.near_plane = camera.at("near").get<float>();
    document.camera.far_plane = camera.at("far").get<float>();
    document.camera.aperture_radius = camera.value("aperture_radius", 0.0F);
    document.camera.focus_distance = camera.value("focus_distance", 4.0F);
    const auto& light = json.at("light");
    document.light.direction = vector(light.at("direction"));
    document.light.color = vector(light.at("color"));
    document.light.intensity = light.at("intensity").get<float>();
    document.ambient = vector(json.at("ambient"));
    const auto& trace = json.at("trace");
    document.trace.epsilon = trace.at("epsilon").get<double>();
    document.trace.relative_epsilon = trace.at("relative_epsilon").get<double>();
    document.trace.normal_step = trace.at("normal_step").get<double>();
    document.trace.maximum_distance = trace.at("maximum_distance").get<double>();
    document.trace.maximum_steps = step_count(trace.at("maximum_steps"));
    document.trace.shadow_steps = step_count(trace.at("shadow_steps"));
    document.trace.safety_factor = trace.at("safety_factor").get<double>();
    document.validate(cancellation);
    return document;
}

void save_sdf(const SdfDocument& document, const std::filesystem::path& path,
              CancellationToken cancellation) {
    document.validate(cancellation);
    Json nodes = Json::array();
    for (const auto& node : document.nodes) {
        cancellation.check();
        Json value{{"id", node.id},
                   {"name", node.name},
                   {"kind", sdf_kind_name(node.kind)},
                   {"parameters", array(node.parameters)},
                   {"translation", array(node.translation)},
                   {"rotation", array(node.rotation_degrees)},
                   {"scale", node.scale},
                   {"color", array(node.color)}};
        if (node.left != invalid_sdf_node) {
            value["left"] = document.nodes[node.left].id;
        }
        if (node.right != invalid_sdf_node) {
            value["right"] = document.nodes[node.right].id;
        }
        nodes.push_back(std::move(value));
    }
    const auto& camera = document.camera;
    const auto& trace = document.trace;
    Json json{{"format", "minirender-sdf"},
              {"version", 1},
              {"root", document.nodes[document.root].id},
              {"nodes", std::move(nodes)},
              {"camera",
               {{"position", array(camera.position)},
                {"target", array(camera.target)},
                {"up", array(camera.up)},
                {"vertical_fov", camera.vertical_fov_degrees},
                {"near", camera.near_plane},
                {"far", camera.far_plane},
                {"aperture_radius", camera.aperture_radius},
                {"focus_distance", camera.focus_distance}}},
              {"light",
               {{"direction", array(document.light.direction)},
                {"color", array(document.light.color)},
                {"intensity", document.light.intensity}}},
              {"ambient", array(document.ambient)},
              {"trace",
               {{"epsilon", trace.epsilon},
                {"relative_epsilon", trace.relative_epsilon},
                {"normal_step", trace.normal_step},
                {"maximum_distance", trace.maximum_distance},
                {"maximum_steps", trace.maximum_steps},
                {"shadow_steps", trace.shadow_steps},
                {"safety_factor", trace.safety_factor}}}};
    detail::StagedFile staged(path);
    staged.stream() << json.dump(2) << '\n';
    cancellation.check();
    staged.commit();
}

} // namespace minirender
