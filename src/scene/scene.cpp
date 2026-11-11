#include <minirender/core/sampling.hpp>
#include <minirender/geometry/uv_tangents.hpp>
#include <minirender/scene/scene.hpp>
#include <minirender/simulation/cloth_component.hpp>

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

void MeshData::validate(CancellationToken cancellation) const {
    cancellation.check();
    require(!positions.empty() && !triangles.empty(), "Mesh must contain positions and triangles");
    std::size_t checked = 0;
    for (const auto position : positions) {
        if (checked++ % 1024 == 0) {
            cancellation.check();
        }
        require(finite(position), "Mesh position must be finite");
    }
    for (const auto& triangle : triangles) {
        if (checked++ % 1024 == 0) {
            cancellation.check();
        }
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
    cancellation.check();
}

void MeshValidationCache::validate(const std::shared_ptr<const MeshData>& mesh,
                                   CancellationToken cancellation) {
    cancellation.check();
    require(mesh != nullptr, "Mesh data is missing");
    std::erase_if(meshes_, [](const auto& value) { return value.expired(); });
    if (!meshes_.contains(mesh)) {
        mesh->validate(cancellation);
        meshes_.insert(mesh);
    }
}

void MeshValidationCache::validate_cloth_uv(const std::shared_ptr<const MeshData>& mesh,
                                            CancellationToken cancellation) {
    validate(mesh, cancellation);
    std::erase_if(cloth_uv_meshes_, [](const auto& value) { return value.expired(); });
    if (cloth_uv_meshes_.contains(mesh)) {
        return;
    }
    for (std::size_t face = 0; face < mesh->triangles.size(); ++face) {
        if (face % 512 == 0) {
            cancellation.check();
        }
        std::array<glm::vec3, 3> positions;
        std::array<glm::vec2, 3> coordinates;
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const auto& value = mesh->triangles[face].corners[corner];
            positions[corner] = mesh->positions[value.position];
            coordinates[corner] = value.uv;
        }
        require(uv_tangents(positions, coordinates).has_value(),
                "Cloth material requires nondegenerate mesh UV coordinates");
    }
    cancellation.check();
    cloth_uv_meshes_.insert(mesh);
}

void MeshValidationCache::merge_validated(const MeshValidationCache& other) {
    std::erase_if(meshes_, [](const auto& value) { return value.expired(); });
    for (const auto& mesh : other.meshes_) {
        if (!mesh.expired()) {
            meshes_.insert(mesh);
        }
    }
    std::erase_if(cloth_uv_meshes_, [](const auto& value) { return value.expired(); });
    for (const auto& mesh : other.cloth_uv_meshes_) {
        if (!mesh.expired()) {
            cloth_uv_meshes_.insert(mesh);
        }
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
    // 合法的大缩放仍可能使 float 行列式溢出，导致逆矩阵全零或 NaN。
    // 用 double 完成逆转置，再存回场景统一使用的 float 矩阵。
    return glm::mat3(glm::transpose(glm::inverse(glm::dmat3(matrix()))));
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
    require(std::isfinite(aperture_radius) && aperture_radius >= 0 &&
                std::isfinite(focus_distance) && focus_distance > 0,
            "Camera lens requires nonnegative aperture and positive focus distance");
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

Ray Camera::generate_ray(glm::dvec2 pixel_uv, double aspect) const {
    validate();
    require(std::isfinite(aspect) && aspect > 0 && std::isfinite(pixel_uv.x) &&
                std::isfinite(pixel_uv.y),
            "Ray coordinates and aspect must be valid");
    const auto forward = glm::normalize(glm::dvec3(target) - glm::dvec3(position));
    const auto right = glm::normalize(glm::cross(forward, glm::dvec3(up)));
    const auto camera_up = glm::cross(right, forward);
    const double half_height =
        std::tan(glm::radians(static_cast<double>(vertical_fov_degrees)) * 0.5);
    const auto direction = forward + (2 * pixel_uv.x - 1) * aspect * half_height * right +
                           (1 - 2 * pixel_uv.y) * half_height * camera_up;
    return {position, glm::normalize(direction)};
}

Ray Camera::generate_lens_ray(glm::dvec2 pixel_uv, double aspect, glm::dvec2 lens_u) const {
    const auto pinhole = generate_ray(pixel_uv, aspect);
    if (aperture_radius == 0) {
        return pinhole;
    }
    const auto disk = sample_disk(lens_u) * static_cast<double>(aperture_radius);
    const auto forward = glm::normalize(glm::dvec3(target) - glm::dvec3(position));
    const auto right = glm::normalize(glm::cross(forward, glm::dvec3(up)));
    const auto camera_up = glm::cross(right, forward);
    const auto focus = pinhole.origin + pinhole.direction * (static_cast<double>(focus_distance) /
                                                             glm::dot(pinhole.direction, forward));
    const auto origin = glm::dvec3(position) + disk.x * right + disk.y * camera_up;
    return {origin, glm::normalize(focus - origin)};
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
    case MaterialKind::cloth:
        return "cloth";
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
    if (name == "cloth") {
        return MaterialKind::cloth;
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
    if (kind == MaterialKind::cloth) {
        require(cloth != nullptr && ior >= 1 && ior <= 3,
                "Cloth material requires appearance data and IOR in 1..3");
    }
    material_kind_name(kind);
    texture_filter_name(texture_filter);
}

void SceneDocument::validate(MeshValidationCache* cache, CancellationToken cancellation) const {
    cancellation.check();
    camera.validate();
    require(finite(environment) && glm::all(glm::greaterThanEqual(environment, glm::vec3(0))),
            "Environment must be finite and nonnegative");
    std::unordered_set<std::string> texture_ids;
    for (const auto& texture : textures) {
        cancellation.check();
        unique_id(texture_ids, texture.id, "Texture");
        require(texture.data != nullptr, "Texture data is missing: " + texture.id);
    }
    require(environment_map.texture_id.empty() || texture_ids.contains(environment_map.texture_id),
            "Environment references missing texture: " + environment_map.texture_id);
    require(std::isfinite(environment_map.strength) && environment_map.strength >= 0 &&
                std::isfinite(environment_map.rotation_degrees),
            "Environment strength and rotation must be finite; strength must be nonnegative");
    std::unordered_set<std::string> material_ids;
    std::unordered_set<std::string> cloth_material_ids;
    for (const auto& material : materials) {
        cancellation.check();
        unique_id(material_ids, material.id, "Material");
        material.validate();
        if (material.kind == MaterialKind::cloth) {
            cloth_material_ids.insert(material.id);
        }
        require(material.base_color_texture.empty() ||
                    texture_ids.contains(material.base_color_texture),
                "Material references missing texture: " + material.base_color_texture);
    }
    std::unordered_set<std::string> mesh_ids;
    for (const auto& mesh : meshes) {
        cancellation.check();
        unique_id(mesh_ids, mesh.id, "Mesh");
        require(mesh.data != nullptr, "Mesh data is missing: " + mesh.id);
        if (cache) {
            cache->validate(mesh.data, cancellation);
        } else {
            mesh.data->validate(cancellation);
        }
    }
    std::unordered_set<std::string> object_ids;
    MeshValidationCache local_cloth_cache;
    for (const auto& object : objects) {
        cancellation.check();
        unique_id(object_ids, object.id, "Object");
        require(mesh_ids.contains(object.mesh_id),
                "Object references missing mesh: " + object.mesh_id);
        require(material_ids.contains(object.material_id),
                "Object references missing material: " + object.material_id);
        object.transform.validate();
        if (object.cloth) {
            const auto mesh = std::find_if(meshes.begin(), meshes.end(), [&](const auto& value) {
                return value.id == object.mesh_id;
            });
            require(mesh->data == object.cloth->mesh(),
                    "Cloth state and scene mesh differ; freeze before changing geometry");
            cloth_environment(*object.cloth->definition(), *this);
        }
        if (object.cloth_collider) {
            object.cloth_collider->validate();
            if (object.cloth_collider->enabled &&
                object.cloth_collider->kind == ClothColliderKind::sphere) {
                const auto scale = glm::abs(object.transform.scale);
                require(std::abs(scale.x - scale.y) <= 1e-5F * scale.x &&
                            std::abs(scale.x - scale.z) <= 1e-5F * scale.x,
                        "Sphere cloth collider requires uniform scale");
            }
        }
        if (cloth_material_ids.contains(object.material_id)) {
            const auto mesh = std::find_if(meshes.begin(), meshes.end(), [&](const auto& value) {
                return value.id == object.mesh_id;
            });
            (cache ? cache : &local_cloth_cache)->validate_cloth_uv(mesh->data, cancellation);
        }
    }
    std::unordered_set<std::string> light_ids;
    for (const auto& light : directional_lights) {
        cancellation.check();
        unique_id(light_ids, light.id, "Light");
        require(finite(light.direction) && glm::length(light.direction) > 1e-6F,
                "Light direction must be nonzero");
        require(finite(light.color) && glm::all(glm::greaterThanEqual(light.color, glm::vec3(0))) &&
                    std::isfinite(light.intensity) && light.intensity >= 0,
                "Light color/intensity must be nonnegative");
    }
}

std::shared_ptr<const SceneSnapshot> make_snapshot(const SceneDocument& document,
                                                   MeshValidationCache* cache) {
    document.validate(cache);
    auto snapshot = std::make_shared<SceneSnapshot>();
    snapshot->camera = document.camera;
    snapshot->environment = document.environment;
    snapshot->directional_lights = document.directional_lights;
    snapshot->environment_strength = document.environment_map.strength;
    snapshot->environment_rotation_degrees = document.environment_map.rotation_degrees;
    for (const auto& texture : document.textures) {
        if (texture.id == document.environment_map.texture_id) {
            snapshot->environment_texture = texture.data;
        }
    }
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
        std::shared_ptr<const Texture> texture;
        for (const auto& asset : document.textures) {
            if (asset.id == material->base_color_texture) {
                texture = asset.data;
            }
        }
        snapshot->instances.push_back({object.id, mesh->data, *material, object.transform.matrix(),
                                       object.transform.normal_matrix(), texture});
    }
    return snapshot;
}

} // namespace minirender
