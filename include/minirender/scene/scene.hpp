#pragma once

#include <glm/glm.hpp>
#include <minirender/core/texture.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace minirender {

// 世界空间射线。direction 为单位向量，t 的单位为米。
struct Ray {
    glm::vec3 origin{0.0F};
    glm::vec3 direction{0.0F, 0.0F, -1.0F};
};

// 面角属性独立于共享的位置索引，保留 UV 接缝和硬法线而不破坏几何拓扑。
struct MeshCorner {
    std::uint32_t position = 0;
    glm::vec3 normal{0.0F, 1.0F, 0.0F};
    glm::vec2 uv{0.0F};
};

struct MeshTriangle {
    std::array<MeshCorner, 3> corners;
};

// MeshData 是可导出的 CPU 网格数据；发布到资产系统后以 const shared_ptr 共享。
struct MeshData {
    std::vector<glm::vec3> positions;
    std::vector<MeshTriangle> triangles;
    void validate() const;
};

// T * Rz * Ry * Rx * S；旋转在界面/文件中以度表示，渲染矩阵按弧度计算。
struct Transform {
    glm::vec3 translation{0.0F};
    glm::vec3 rotation_degrees{0.0F};
    glm::vec3 scale{1.0F};
    glm::mat4 matrix() const;
    glm::mat3 normal_matrix() const;
    void validate() const;
};

// 右手相机，目标点决定 -Z 方向。像素 uv 原点在左上角。
// Vulkan 投影包含一次 Y 翻转，射线生成与其使用同一 FOV/朝向约定。
struct Camera {
    glm::vec3 position{0.0F, 1.5F, 4.0F};
    glm::vec3 target{0.0F, 0.5F, 0.0F};
    glm::vec3 up{0.0F, 1.0F, 0.0F};
    float vertical_fov_degrees = 45.0F;
    float near_plane = 0.05F;
    float far_plane = 100.0F;
    void validate() const;
    glm::mat4 view_matrix() const;
    glm::mat4 projection_matrix(float aspect) const;
    Ray generate_ray(glm::vec2 pixel_uv, float aspect) const;
};

enum class MaterialKind { diffuse, conductor, mirror, dielectric };
std::string material_kind_name(MaterialKind kind);
MaterialKind parse_material_kind(const std::string& name);

// base_color、emission 均为线性 RGB。底色纹理通过独立资产 ID 引用。
struct Material {
    std::string id;
    std::string name;
    MaterialKind kind = MaterialKind::diffuse;
    glm::vec3 base_color{0.7F};
    glm::vec3 emission{0.0F};
    float roughness = 0.35F;
    float ior = 1.5F;
    std::string base_color_texture{};
    TextureFilter texture_filter = TextureFilter::trilinear;
    void validate() const;
};

struct MeshAsset {
    std::string id;
    std::filesystem::path source; // 相对于 asset_directory；导入时可为绝对路径，保存时转换。
    std::shared_ptr<const MeshData> data;
};

struct TextureAsset {
    std::string id;
    std::filesystem::path source;
    ColorEncoding encoding = ColorEncoding::srgb;
    std::shared_ptr<const Texture> data;
};

struct SceneObject {
    std::string id;
    std::string name;
    std::string mesh_id;
    std::string material_id;
    Transform transform;
    bool visible = true;
};

// direction 表示光传播方向；着色点朝向光源的方向为 -normalize(direction)。
struct DirectionalLight {
    std::string id;
    glm::vec3 direction{-1.0F, -2.0F, -1.0F};
    glm::vec3 color{1.0F};
    float intensity = 3.0F;
};

// UI 编辑的数据。每次开始渲染都创建值拷贝快照，避免后台读取可变控件状态。
struct SceneDocument {
    std::filesystem::path asset_directory;
    Camera camera;
    glm::vec3 environment{0.03F};
    std::vector<Material> materials;
    std::vector<MeshAsset> meshes;
    std::vector<TextureAsset> textures;
    std::vector<SceneObject> objects;
    std::vector<DirectionalLight> directional_lights;
    void validate() const;
};

// 每个实例持有独立的材质/变换值和只读网格。用户编辑原文档不改变已经开始的任务。
struct RenderInstance {
    std::string object_id;
    std::shared_ptr<const MeshData> mesh;
    Material material;
    glm::mat4 object_to_world{1.0F};
    glm::mat3 normal_to_world{1.0F};
    std::shared_ptr<const Texture> base_color_texture;
};

struct SceneSnapshot {
    Camera camera;
    glm::vec3 environment;
    std::vector<RenderInstance> instances;
    std::vector<DirectionalLight> directional_lights;
};

std::shared_ptr<const SceneSnapshot> make_snapshot(const SceneDocument& document);

} // namespace minirender
