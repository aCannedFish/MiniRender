#include <minirender/scene/scene.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace minirender {
namespace {

bool finite(glm::vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

// 场景错误尽量在编辑/导入边界报告，不让非法参数在 GPU 上变为 NaN 或越界访问。
void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::invalid_argument(message);
    }
}

void unique_id(std::unordered_set<std::string>& ids, const std::string& id, const char* type) {
    require(!id.empty() && ids.insert(id).second,
            std::string(type) + " ID is empty or duplicated: " + id);
}

} // namespace

void MeshData::validate() const {
    require(!positions.empty() && !triangles.empty(), "Mesh must contain positions and triangles");
    for (const auto position : positions) {
        require(finite(position), "Mesh position must be finite");
    }
    for (const auto& triangle : triangles) {
        for (const auto& corner : triangle.corners) {
            require(corner.position < positions.size(), "Mesh position index is out of range");
            require(finite(corner.normal) && glm::dot(corner.normal, corner.normal) > 1e-12F,
                    "Mesh normal must be finite and nonzero");
            require(std::isfinite(corner.uv.x) && std::isfinite(corner.uv.y),
                    "Mesh UV must be finite");
        }
        const auto a = positions[triangle.corners[0].position];
        const auto b = positions[triangle.corners[1].position];
        const auto c = positions[triangle.corners[2].position];
        const auto area = glm::cross(b - a, c - a);
        require(glm::dot(area, area) > 1e-20F, "Degenerate mesh triangle");
    }
}

void Transform::validate() const {
    require(finite(translation) && finite(rotation_degrees) && finite(scale),
            "Transform must be finite");
    require(std::abs(scale.x) > 1e-6F && std::abs(scale.y) > 1e-6F && std::abs(scale.z) > 1e-6F,
            "Transform scale must be invertible");
}

glm::mat4 Transform::matrix() const {
    validate();
    glm::mat4 result = glm::translate(glm::mat4(1.0F), translation);
    result = glm::rotate(result, glm::radians(rotation_degrees.z), {0, 0, 1});
    result = glm::rotate(result, glm::radians(rotation_degrees.y), {0, 1, 0});
    result = glm::rotate(result, glm::radians(rotation_degrees.x), {1, 0, 0});
    return glm::scale(result, scale);
}

glm::mat3 Transform::normal_matrix() const {
    return glm::transpose(glm::inverse(glm::mat3(matrix())));
}

void Camera::validate() const {
    require(finite(position) && finite(target) && finite(up), "Camera vectors must be finite");
    require(glm::length(target - position) > 1e-6F, "Camera position and target must differ");
    require(glm::length(glm::cross(target - position, up)) > 1e-6F,
            "Camera up must not be parallel to view direction");
    require(std::isfinite(vertical_fov_degrees) && vertical_fov_degrees > 0 &&
                vertical_fov_degrees < 179,
            "Camera FOV must be between 0 and 179 degrees");
    require(std::isfinite(near_plane) && std::isfinite(far_plane) && near_plane > 0 &&
                far_plane > near_plane,
            "Camera clipping planes must satisfy 0 < near < far");
}

glm::mat4 Camera::view_matrix() const {
    validate();
    return glm::lookAtRH(position, target, up);
}

glm::mat4 Camera::projection_matrix(float aspect) const {
    validate();
    require(std::isfinite(aspect) && aspect > 0, "Camera aspect must be positive");
    auto projection =
        glm::perspectiveRH_ZO(glm::radians(vertical_fov_degrees), aspect, near_plane, far_plane);
    projection[1][1] *= -1.0F;
    return projection;
}

Ray Camera::generate_ray(glm::vec2 pixel_uv, float aspect) const {
    validate();
    require(std::isfinite(aspect) && aspect > 0 && std::isfinite(pixel_uv.x) &&
                std::isfinite(pixel_uv.y),
            "Ray coordinates and aspect must be valid");
    const auto forward = glm::normalize(target - position);
    const auto right = glm::normalize(glm::cross(forward, up));
    const auto camera_up = glm::cross(right, forward);
    const float half_height = std::tan(glm::radians(vertical_fov_degrees) * 0.5F);
    const auto direction = forward + (2.0F * pixel_uv.x - 1.0F) * aspect * half_height * right +
                           (1.0F - 2.0F * pixel_uv.y) * half_height * camera_up;
    return {position, glm::normalize(direction)};
}

std::string material_kind_name(MaterialKind kind) {
    switch (kind) {
    case MaterialKind::diffuse:
        return "diffuse";
    case MaterialKind::conductor:
        return "conductor";
    case MaterialKind::mirror:
        return "mirror";
    case MaterialKind::dielectric:
        return "dielectric";
    }
    throw std::invalid_argument("Invalid material kind");
}

MaterialKind parse_material_kind(const std::string& name) {
    if (name == "diffuse") {
        return MaterialKind::diffuse;
    }
    if (name == "conductor") {
        return MaterialKind::conductor;
    }
    if (name == "mirror") {
        return MaterialKind::mirror;
    }
    if (name == "dielectric") {
        return MaterialKind::dielectric;
    }
    throw std::invalid_argument("Unsupported material kind: " + name);
}

void Material::validate() const {
    require(finite(base_color) && glm::all(glm::greaterThanEqual(base_color, glm::vec3(0))) &&
                glm::all(glm::lessThanEqual(base_color, glm::vec3(1))),
            "Material base color must be in [0,1]");
    require(finite(emission) && glm::all(glm::greaterThanEqual(emission, glm::vec3(0))),
            "Material emission must be nonnegative");
    require(std::isfinite(roughness) && roughness >= 0 && roughness <= 1,
            "Roughness must be in [0,1]");
    require(std::isfinite(ior) && ior > 0, "IOR must be positive");
    material_kind_name(kind);
}

void SceneDocument::validate() const {
    camera.validate();
    require(finite(environment) && glm::all(glm::greaterThanEqual(environment, glm::vec3(0))),
            "Environment must be finite and nonnegative");
    std::unordered_set<std::string> material_ids;
    for (const auto& material : materials) {
        unique_id(material_ids, material.id, "Material");
        material.validate();
    }
    std::unordered_set<std::string> mesh_ids;
    for (const auto& mesh : meshes) {
        unique_id(mesh_ids, mesh.id, "Mesh");
        require(mesh.data != nullptr, "Mesh data is missing: " + mesh.id);
        mesh.data->validate();
    }
    std::unordered_set<std::string> object_ids;
    for (const auto& object : objects) {
        unique_id(object_ids, object.id, "Object");
        require(mesh_ids.contains(object.mesh_id),
                "Object references missing mesh: " + object.mesh_id);
        require(material_ids.contains(object.material_id),
                "Object references missing material: " + object.material_id);
        object.transform.validate();
    }
    std::unordered_set<std::string> light_ids;
    for (const auto& light : directional_lights) {
        unique_id(light_ids, light.id, "Light");
        require(finite(light.direction) && glm::length(light.direction) > 1e-6F,
                "Light direction must be nonzero");
        require(finite(light.color) && glm::all(glm::greaterThanEqual(light.color, glm::vec3(0))) &&
                    std::isfinite(light.intensity) && light.intensity >= 0,
                "Light color/intensity must be nonnegative");
    }
}

std::shared_ptr<const SceneSnapshot> make_snapshot(const SceneDocument& document) {
    document.validate();
    auto snapshot = std::make_shared<SceneSnapshot>();
    snapshot->camera = document.camera;
    snapshot->environment = document.environment;
    snapshot->directional_lights = document.directional_lights;
    for (const auto& object : document.objects) {
        if (!object.visible) {
            continue;
        }
        const auto mesh =
            std::find_if(document.meshes.begin(), document.meshes.end(),
                         [&](const auto& asset) { return asset.id == object.mesh_id; });
        const auto material =
            std::find_if(document.materials.begin(), document.materials.end(),
                         [&](const auto& value) { return value.id == object.material_id; });
        snapshot->instances.push_back({object.id, mesh->data, *material, object.transform.matrix(),
                                       object.transform.normal_matrix()});
    }
    return snapshot;
}

} // namespace minirender
