#include <minirender/assets/scene_io.hpp>

#include <nlohmann/json.hpp>

#include <fstream>
#include <iterator>
#include <stdexcept>

namespace minirender {
namespace {

using Json = nlohmann::json;

glm::vec3 vector3(const Json& json) {
    if (!json.is_array() || json.size() != 3) {
        throw std::invalid_argument("Expected a three-component vector");
    }
    return {json.at(0).get<float>(), json.at(1).get<float>(), json.at(2).get<float>()};
}

Json vector3(glm::vec3 value) {
    return Json::array({value.x, value.y, value.z});
}

// 保留 UTF-8 JSON 路径到 filesystem 的显式转换，避免 Windows 当前代码页损失字符。
std::filesystem::path path_from_utf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

// Windows 跨盘符无法生成相对路径。此时复制资产到新场景旁，仍保持场景可移动。
// 不覆盖已有不同内容的文件；同内容复用，冲突时选择新的编号。
std::filesystem::path portable_asset_path(const std::filesystem::path& source,
                                          const std::filesystem::path& destination) {
    const auto relative = std::filesystem::relative(source, destination);
    if (!relative.empty()) {
        return relative;
    }
    const auto assets = destination / "assets";
    std::filesystem::create_directories(assets);
    std::ifstream input(source, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot copy scene asset: " + path_to_utf8(source));
    }
    const std::string original((std::istreambuf_iterator<char>(input)), {});
    for (unsigned index = 1;; ++index) {
        auto name = std::filesystem::path("asset-" + std::to_string(index));
        name += source.extension();
        const auto candidate = assets / name;
        if (std::filesystem::exists(candidate)) {
            if (std::filesystem::file_size(candidate) != original.size()) {
                continue;
            }
            std::ifstream existing(candidate, std::ios::binary);
            if (!existing) {
                continue;
            }
            const std::string content((std::istreambuf_iterator<char>(existing)), {});
            if (content != original) {
                continue;
            }
        } else {
            std::filesystem::copy_file(source, candidate);
        }
        return std::filesystem::path("assets") / name;
    }
}

} // namespace

SceneDocument load_scene(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) {
        throw std::runtime_error("Cannot open scene: " + path_to_utf8(path));
    }
    try {
        Json json;
        stream >> json;
        if (!json.at("version").is_number_integer() || json.at("version").get<int>() != 1) {
            throw std::invalid_argument("Unsupported scene version");
        }
        SceneDocument scene;
        scene.asset_directory = std::filesystem::absolute(path).parent_path();
        const auto& camera = json.at("camera");
        scene.camera.position = vector3(camera.at("position"));
        scene.camera.target = vector3(camera.at("target"));
        scene.camera.up = vector3(camera.at("up"));
        scene.camera.vertical_fov_degrees = camera.at("vertical_fov").get<float>();
        scene.camera.near_plane = camera.at("near").get<float>();
        scene.camera.far_plane = camera.at("far").get<float>();
        scene.environment = vector3(json.at("environment"));
        if (json.contains("environment_map")) {
            const auto& environment = json.at("environment_map");
            scene.environment_map.texture_id = environment.value("texture", std::string{});
            scene.environment_map.strength = environment.value("strength", 1.0F);
            scene.environment_map.rotation_degrees = environment.value("rotation_degrees", 0.0F);
        }
        for (const auto& value : json.at("materials")) {
            Material material;
            material.id = value.at("id").get<std::string>();
            material.name = value.value("name", material.id);
            material.kind = parse_material_kind(value.at("kind").get<std::string>());
            material.base_color = vector3(value.at("base_color"));
            material.emission = vector3(value.value("emission", Json::array({0, 0, 0})));
            material.roughness = value.value("roughness", 0.35F);
            material.ior = value.value("ior", 1.5F);
            material.base_color_texture = value.value("base_color_texture", std::string{});
            material.texture_filter =
                parse_texture_filter(value.value("texture_filter", std::string("trilinear")));
            scene.materials.push_back(material);
        }
        for (const auto& value : json.at("meshes")) {
            MeshAsset mesh;
            mesh.id = value.at("id").get<std::string>();
            mesh.source = path_from_utf8(value.at("source").get<std::string>());
            mesh.data =
                std::make_shared<const MeshData>(load_obj(scene.asset_directory / mesh.source));
            scene.meshes.push_back(std::move(mesh));
        }
        for (const auto& value : json.value("textures", Json::array())) {
            TextureAsset texture;
            texture.id = value.at("id").get<std::string>();
            texture.source = path_from_utf8(value.at("source").get<std::string>());
            const auto encoding = value.value("encoding", std::string("srgb"));
            if (encoding != "srgb" && encoding != "linear") {
                throw std::invalid_argument("Unknown texture encoding");
            }
            texture.encoding = encoding == "srgb" ? ColorEncoding::srgb : ColorEncoding::linear;
            texture.data = std::make_shared<const Texture>(
                Image::read(scene.asset_directory / texture.source, texture.encoding));
            scene.textures.push_back(std::move(texture));
        }
        for (const auto& value : json.at("objects")) {
            SceneObject object;
            object.id = value.at("id").get<std::string>();
            object.name = value.value("name", object.id);
            object.mesh_id = value.at("mesh").get<std::string>();
            object.material_id = value.at("material").get<std::string>();
            object.visible = value.value("visible", true);
            const auto& transform = value.at("transform");
            object.transform.translation = vector3(transform.at("translation"));
            object.transform.rotation_degrees = vector3(transform.at("rotation"));
            object.transform.scale = vector3(transform.at("scale"));
            scene.objects.push_back(object);
        }
        for (const auto& value : json.at("directional_lights")) {
            scene.directional_lights.push_back(
                {value.at("id").get<std::string>(), vector3(value.at("direction")),
                 vector3(value.at("color")), value.at("intensity").get<float>()});
        }
        scene.validate();
        return scene;
    } catch (const std::exception& error) {
        throw std::runtime_error("Scene " + path_to_utf8(path) + ": " + error.what());
    }
}

void save_scene(const SceneDocument& document, const std::filesystem::path& path) {
    document.validate();
    Json json;
    json["version"] = 1;
    const auto& camera = document.camera;
    json["camera"] = {{"position", vector3(camera.position)},
                      {"target", vector3(camera.target)},
                      {"up", vector3(camera.up)},
                      {"vertical_fov", camera.vertical_fov_degrees},
                      {"near", camera.near_plane},
                      {"far", camera.far_plane}};
    json["environment"] = vector3(document.environment);
    json["environment_map"] = {{"texture", document.environment_map.texture_id},
                               {"strength", document.environment_map.strength},
                               {"rotation_degrees", document.environment_map.rotation_degrees}};
    for (const auto* key : {"materials", "meshes", "textures", "objects", "directional_lights"}) {
        json[key] = Json::array();
    }
    for (const auto& material : document.materials) {
        json["materials"].push_back(
            {{"id", material.id},
             {"name", material.name},
             {"kind", material_kind_name(material.kind)},
             {"base_color", vector3(material.base_color)},
             {"emission", vector3(material.emission)},
             {"roughness", material.roughness},
             {"ior", material.ior},
             {"base_color_texture", material.base_color_texture},
             {"texture_filter", texture_filter_name(material.texture_filter)}});
    }
    const auto destination_directory = std::filesystem::absolute(path).parent_path();
    for (const auto& mesh : document.meshes) {
        if (mesh.source.empty()) {
            throw std::invalid_argument("Mesh must be exported before saving scene: " + mesh.id);
        }
        const auto relative =
            portable_asset_path(document.asset_directory / mesh.source, destination_directory);
        json["meshes"].push_back({{"id", mesh.id}, {"source", path_to_utf8(relative)}});
    }
    for (const auto& texture : document.textures) {
        if (texture.source.empty()) {
            throw std::invalid_argument("Texture must have a source before saving");
        }
        const auto relative =
            portable_asset_path(document.asset_directory / texture.source, destination_directory);
        json["textures"].push_back(
            {{"id", texture.id},
             {"source", path_to_utf8(relative)},
             {"encoding", texture.encoding == ColorEncoding::srgb ? "srgb" : "linear"}});
    }
    for (const auto& object : document.objects) {
        const auto& transform = object.transform;
        json["objects"].push_back({{"id", object.id},
                                   {"name", object.name},
                                   {"mesh", object.mesh_id},
                                   {"material", object.material_id},
                                   {"visible", object.visible},
                                   {"transform",
                                    {{"translation", vector3(transform.translation)},
                                     {"rotation", vector3(transform.rotation_degrees)},
                                     {"scale", vector3(transform.scale)}}}});
    }
    for (const auto& light : document.directional_lights) {
        json["directional_lights"].push_back({{"id", light.id},
                                              {"direction", vector3(light.direction)},
                                              {"color", vector3(light.color)},
                                              {"intensity", light.intensity}});
    }
    std::ofstream stream(path);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream << json.dump(2) << '\n';
    stream.close();
}

} // namespace minirender
