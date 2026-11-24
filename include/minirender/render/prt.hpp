// 在静态三角网格上烘焙九项 RGB 球谐传输，并与可旋转的环境光系数点乘。
#pragma once
#include <functional>
#include <minirender/render/spherical_harmonics.hpp>
#include <minirender/scene/scene.hpp>

namespace minirender {
enum class PrtMode { unoccluded, shadowed, interreflection };

struct PrtSettings {
    PrtMode       mode    = PrtMode::shadowed;
    std::uint32_t samples = 128, bounces = 1;
    std::uint64_t seed                        = 202;
    bool operator==(const PrtSettings&) const = default;
};

using PrtTransfer = std::array<glm::vec3, 9>;

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
glm::vec3 prt_irradiance(const PrtTransfer& transfer, const ShLight& light);
} // namespace minirender
