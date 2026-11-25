// 场景、网格、材质、相机与不可变渲染快照的数据约定。

#pragma once

#include <glm/glm.hpp>
#include <minirender/core/texture.hpp>
#include <minirender/scene/cloth_collider.hpp>
#include <minirender/scene/cloth_material.hpp>
#include <minirender/scene/medium.hpp>
#include <minirender/scene/subsurface.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace minirender {

class ClothComponent;

// CPU 世界空间射线。使用 double 保留光源采样方向和有限阴影线段的距离精度。
// direction 为单位向量，t 的单位为米；网格存储和 GPU 数据仍采用 float。
struct Ray {
    glm::dvec3 origin{0.0};
    glm::dvec3 direction{0.0, 0.0, -1.0};
};

// 面角属性独立于共享的位置索引，保留 UV 接缝和硬法线而不破坏几何拓扑。
struct MeshCorner {
    std::uint32_t position = 0;
    glm::vec3     normal{0.0F, 1.0F, 0.0F};
    glm::vec2     uv{0.0F};
};

// 按绕序保存三个面角，面角可分别引用同一位置上的不同 UV 和法线。
struct MeshTriangle {
    std::array<MeshCorner, 3> corners;
};

// MeshData 是可导出的 CPU 网格数据；发布到资产系统后以 const shared_ptr 共享。
struct MeshData {
    std::vector<glm::vec3>    positions;
    std::vector<MeshTriangle> triangles;
    // 检查坐标、面角索引、法线、UV 与三角形有效性。
    void validate(CancellationToken cancellation = {}) const;
};

// 资产发布后不可再修改。按 shared_ptr 的所有权缓存校验结果，过期弱引用及时清理。
// 缓存仅在所属编辑线程使用；新网格即使复用原始地址也会按新的所有权重新校验。
class MeshValidationCache {
  public:
    // 仅在此网格所有者首次出现时执行完整数据校验。
    void validate(const std::shared_ptr<const MeshData>& mesh, CancellationToken cancellation = {});
    void validate_cloth_uv(const std::shared_ptr<const MeshData>& mesh,
                           CancellationToken                      cancellation = {});
    // 合并后台已完成的校验结果；私有条目只能由 validate 成功后创建。
    // 调用方须保证两个缓存此时均无其他线程写入，网格仍遵守发布后不可变约定。
    void merge_validated(const MeshValidationCache& other);

  private:
    std::set<std::weak_ptr<const MeshData>, std::owner_less<std::weak_ptr<const MeshData>>> meshes_;
    std::set<std::weak_ptr<const MeshData>, std::owner_less<std::weak_ptr<const MeshData>>>
        cloth_uv_meshes_;
};

// T * Rz * Ry * Rx * S；旋转在界面/文件中以度表示，渲染矩阵按弧度计算。
struct Transform {
    glm::vec3 translation{0.0F};
    glm::vec3 rotation_degrees{0.0F};
    glm::vec3 scale{1.0F};
    // 生成局部到世界矩阵，物体与碰撞几何共用同一变换。
    glm::mat4 matrix() const;
    // 返回线性变换的逆转置，保持非均匀缩放后的法线方向。
    glm::mat3 normal_matrix() const;
    // 要求变换分量有限且缩放可逆，允许镜像缩放。
    void validate() const;
};

// 右手相机，目标点决定 -Z 方向。像素 uv 原点在左上角。
// Vulkan 投影包含一次 Y 翻转，射线生成与其使用同一 FOV/朝向约定。
struct Camera {
    glm::vec3 position{0.0F, 1.5F, 4.0F};
    glm::vec3 target{0.0F, 0.5F, 0.0F};
    glm::vec3 up{0.0F, 1.0F, 0.0F};
    float     vertical_fov_degrees = 45.0F;
    float     near_plane           = 0.05F;
    float     far_plane            = 100.0F;
    float     aperture_radius      = 0;
    float     focus_distance       = 4;
    // 检查视线、上方向、视场、裁剪面与透镜参数。
    void validate() const;
    // 由世界空间位置、目标和上方向构建相机视图。
    glm::mat4 view_matrix() const;
    // 按宽高比构建深度范围为 [0,1] 的 Vulkan 透视投影。
    glm::mat4 projection_matrix(float aspect) const;
    // 从针孔相机穿过归一化像素坐标发射单位方向射线。
    Ray generate_ray(glm::dvec2 pixel_uv, double aspect) const;
    // focus_distance 沿相机 forward 测量。孔径为零时严格返回针孔射线。
    Ray generate_lens_ray(glm::dvec2 pixel_uv, double aspect, glm::dvec2 lens_u) const;
};

enum class MaterialKind { diffuse, conductor, mirror, dielectric, cloth, subsurface };
// 生成场景 JSON 中持久化的材质类型名。
std::string material_kind_name(MaterialKind kind);
// 从持久化名称恢复枚举，未知名称报告为输入错误。
MaterialKind parse_material_kind(const std::string& name);

// base_color、emission 均为线性 RGB。底色纹理通过独立资产 ID 引用。
struct Material {
    std::string   id;
    std::string   name;
    MaterialKind  kind = MaterialKind::diffuse;
    glm::vec3     base_color{0.7F};
    glm::vec3     emission{0.0F};
    float         roughness = 0.35F;
    float         ior       = 1.5F;
    std::string   base_color_texture{};
    TextureFilter texture_filter = TextureFilter::trilinear;
    // 正面由几何绕序决定，镜像变换保留原外侧。默认双面兼容已有场景。
    // CPU 的 SSS 封闭边界始终保留双面，用于识别内部侧命中。
    bool                                        double_sided = true;
    std::shared_ptr<const ClothAppearance>      cloth{};
    std::shared_ptr<const SubsurfaceParameters> subsurface{};
    // 粗糙导体使用 Kulla–Conty 补偿多次微面散射，旧场景默认为单次散射。
    bool compensate_energy = false;
    // 法线纹理使用线性 RGB。flip_y 用于从 glTF 的 UV 约定转换到项目的 OBJ 坐标。
    std::string normal_texture{};
    float       normal_scale  = 1.F;
    bool        normal_flip_y = false;
    // 校验颜色、粗糙度、折射率及材质类型对应的参数。
    void validate() const;
};

// 网格表条目。多个物体可引用同一 id 并共享不可变顶点数据。
struct MeshAsset {
    std::string           id;
    std::filesystem::path source; // 相对于 asset_directory；导入时可为绝对路径，保存时转换。
    std::shared_ptr<const MeshData> data;
    // 目录导入的资产随场景保存，可将场景文件夹复制到其他设备。
    bool package_on_save = false;
};

// 纹理表条目。encoding 描述源文件通道，data 保存解码后的采样数据。
struct TextureAsset {
    std::string                    id;
    std::filesystem::path          source;
    ColorEncoding                  encoding = ColorEncoding::srgb;
    std::shared_ptr<const Texture> data;
    // 目录导入的资产随场景保存，可将场景文件夹复制到其他设备。
    bool package_on_save = false;
};

// 可编辑物体通过 ID 引用网格与材质，变换和模拟组件由实例独立持有。
struct SceneObject {
    std::string                           id;
    std::string                           name;
    std::string                           mesh_id;
    std::string                           material_id;
    Transform                             transform;
    bool                                  visible = true;
    std::shared_ptr<const ClothComponent> cloth{};
    std::optional<ClothCollider>          cloth_collider{};
};

// direction 表示光传播方向；着色点朝向光源的方向为 -normalize(direction)。
struct DirectionalLight {
    std::string id;
    glm::vec3   direction{-1.0F, -2.0F, -1.0F};
    glm::vec3   color{1.0F};
    float       intensity = 3.0F;
};

// 环境图使用线性 RGB，作为常量 environment 之外的照明。旋转为右手系绕 +Y 的角度。
struct EnvironmentMap {
    std::string texture_id;
    float       strength         = 1;
    float       rotation_degrees = 0;
};

// UI 编辑的数据。每次开始渲染都创建值拷贝快照，避免后台读取可变控件状态。
struct SceneDocument {
    std::filesystem::path         asset_directory;
    Camera                        camera;
    glm::vec3                     environment{0.03F};
    std::vector<Material>         materials;
    std::vector<MeshAsset>        meshes;
    std::vector<TextureAsset>     textures;
    std::vector<SceneObject>      objects;
    std::vector<DirectionalLight> directional_lights;
    EnvironmentMap                environment_map;
    std::vector<MediumRegion>     media{};
    // 检查相机、资产标识及引用关系，按需复用已经校验的网格。
    void validate(MeshValidationCache* cache = nullptr, CancellationToken cancellation = {}) const;
};

// 每个实例持有独立的材质/变换值和只读网格。用户编辑原文档不改变已经开始的任务。
struct RenderInstance {
    std::string                     object_id;
    std::shared_ptr<const MeshData> mesh;
    Material                        material;
    glm::mat4                       object_to_world{1.0F};
    glm::mat3                       normal_to_world{1.0F};
    std::shared_ptr<const Texture>  base_color_texture;
    std::shared_ptr<const Texture>  normal_texture{};
};

// 实时光源视图从可见发光三角形提取。四角以物体正面绕序排列。
struct RectangleLight {
    std::array<glm::vec3, 4> corners;
    glm::vec3                radiance;
    bool                     double_sided;
    std::string              object_id;
};

// 一次渲染读取的不可变输入。实例已解析网格和材质引用，矩形光来自可见发光几何。
struct SceneSnapshot {
    Camera                         camera;
    glm::vec3                      environment;
    std::vector<RenderInstance>    instances;
    std::vector<DirectionalLight>  directional_lights;
    std::shared_ptr<const Texture> environment_texture;
    float                          environment_strength         = 1;
    float                          environment_rotation_degrees = 0;
    std::vector<RectangleLight>    rectangles;
    std::vector<MediumRegion>      media{};
};

// 校验文档，解析可见实例与环境贴图，固定后续采样所需的数据。
std::shared_ptr<const SceneSnapshot> make_snapshot(const SceneDocument& document,
                                                   MeshValidationCache* cache = nullptr);

} // namespace minirender
