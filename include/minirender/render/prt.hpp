// 在静态三角网格上烘焙九项 RGB 球谐传输，并与可旋转的环境光系数点乘。
#pragma once
#include <functional>
#include <minirender/render/spherical_harmonics.hpp>
#include <minirender/scene/scene.hpp>

namespace minirender {
enum class PrtMode { unoccluded, shadowed, interreflection };

// 一个缓存版本对应一组烘焙参数，相机与环境光的方向不参与缓存键。
struct PrtSettings {
    PrtMode       mode                        = PrtMode::shadowed;
    std::uint32_t samples                     = 128; // 每个独立位置和法线的余弦加权样本数。
    std::uint32_t bounces                     = 1;   // 互反射模式允许的额外漫反射次数。
    std::uint64_t seed                        = 202; // 与顶点序号组合，生成可复现的样本序列。
    bool operator==(const PrtSettings&) const = default;
};

// 一个面角在九个实球谐基上的 RGB 响应。
using PrtTransfer = std::array<glm::vec3, 9>;

// 按实例记录起始面角；顶点系数与光栅顶点的顺序一致。
struct PrtBake {
    // 对应 GPU 展平后的实例、三角形和面角。offsets[i] 是实例 i 的首个顶点。
    std::vector<std::uint32_t> offsets;
    std::vector<PrtTransfer>   vertices;
};

// 运行在工作线程。进度单位为三角形面角，取消通过 token 传播。
PrtBake bake_prt(std::shared_ptr<const SceneSnapshot> scene, PrtSettings settings,
                 CancellationToken                             token    = {},
                 std::function<void(std::size_t, std::size_t)> progress = {});
// 比较烘焙依赖。同一组共享网格重新构造的快照可继续使用已有传输数据。
bool compatible_prt(const SceneSnapshot& a, const SceneSnapshot& b, PrtMode mode);
// 根据几何内容、实例变换和烘焙参数生成可跨启动复用的缓存键。
std::uint64_t prt_cache_key(const SceneSnapshot& scene, PrtSettings settings,
                            CancellationToken token = {});
// 环境色与纹理按场景强度相加，方向按环境旋转变换。
ShLight scene_sh_light(const SceneSnapshot& scene);
// 环境与传输逐通道点乘，返回单位漫反射材质受到的环境照明，着色时再乘底色。
glm::vec3 prt_irradiance(const PrtTransfer& transfer, const ShLight& light);
} // namespace minirender
