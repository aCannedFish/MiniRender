#pragma once

#include <minirender/core/cancellation.hpp>
#include <minirender/core/texture.hpp>

#include <functional>
#include <memory>

namespace minirender {

enum class IblStage { source, mipmaps, diffuse, specular, brdf, complete };

// 每个阶段的实际完成像素数。total=0 表示无法逐像素报告的 mip 构建阶段。
// 回调在调用 bake 的线程执行，不得直接访问 UI。
struct IblProgress {
    IblStage stage;
    std::uint64_t completed = 0;
    std::uint64_t total = 0;
};
using IblProgressCallback = std::function<void(const IblProgress&)>;

struct IblEnvironmentSettings {
    std::size_t source_maximum_dimension = 1024;
    std::size_t diffuse_width = 32; // latlong 2:1
    std::size_t specular_width = 128;
    std::size_t specular_layers = 6; // 各层同尺寸，roughness=i/(layers-1)。
    std::uint32_t diffuse_samples = 256;
    std::uint32_t specular_samples = 256;
    std::uint64_t seed = 101;
    void validate() const;
};
struct IblBrdfSettings {
    std::size_t resolution = 64;
    std::uint32_t samples = 512;
    std::uint64_t seed = 101;
    void validate() const;
};

// 完成后以 shared_ptr<const> 发布。烘焙不含常量 environment、强度或旋转，便于缓存。
// source 为非负、有界的普通纹素中心 latlong；烘焙图首末行是北/南极端点，整行相同。
// diffuse 存 E/pi，specular 存 GGX 预滤波辐射，二者采样 Y 坐标为 v*(height-1)。
struct IblEnvironment {
    std::shared_ptr<const Texture> source;
    Image diffuse;
    std::vector<Image> specular;
};

// 经度零为 +X，北极 +Y，经度增加朝 +Z。绕 +Y 的旋转与 EnvironmentLight 一致。
// X repeat、Y clamp，双线性和 LOD 插值均在线性 RGB 中进行；输入方向须为单位向量。
glm::dvec3 sample_ibl_latlong(const Image& image, glm::dvec3 direction,
                              double rotation_degrees = 0);
glm::dvec3 sample_ibl_diffuse(const IblEnvironment& maps, glm::dvec3 normal,
                              double rotation_degrees = 0);
glm::dvec3 sample_ibl_specular(const IblEnvironment& maps, glm::dvec3 reflection, double roughness,
                               double rotation_degrees = 0);
// LUT 像素对应参数网格的端点：x=NdotV，y=roughness；RGB=(A,B,0)。
glm::dvec2 sample_ibl_brdf(const Image& lut, double normal_dot_view, double roughness);

// GGX/Smith G1 乘积/Schlick 的 split-sum 系数，specular=Lp*(F0*A+B)。
// 使用已有 BSDF 的 VNDF 采样，保留无效反射的零贡献，不条件化重新采样。
glm::dvec2 integrate_ibl_brdf(double normal_dot_view, double roughness, std::uint32_t samples,
                              std::uint64_t seed = 101, const CancellationToken& cancellation = {});

std::shared_ptr<const Image> bake_ibl_brdf(IblBrdfSettings settings = {},
                                           CancellationToken cancellation = {},
                                           IblProgressCallback progress = {});
std::shared_ptr<const IblEnvironment> bake_ibl_environment(std::shared_ptr<const Texture> source,
                                                           IblEnvironmentSettings settings = {},
                                                           CancellationToken cancellation = {},
                                                           IblProgressCallback progress = {});

} // namespace minirender
