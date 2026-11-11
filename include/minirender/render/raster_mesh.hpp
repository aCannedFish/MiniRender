#pragma once

#include <minirender/scene/scene.hpp>

#include <functional>
#include <map>
#include <span>

namespace minirender {

// 实时表面管线的交错顶点。数据在 CPU 准备，GPU 只接收连续字节。
struct RasterVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
    // 每个面角保存同一个几何法线，线框据此剔除所属三角形的背面。
    glm::vec3 geometric_normal;
    glm::vec3 tangent_u{0};
    glm::vec3 tangent_v{0};
};
static_assert(sizeof(RasterVertex) == 68);

// 展开面角后，每三项组成一个三角形；edges 每六项给出该面的三条线段。
// 边界为局部坐标 AABB，和源网格包含的全部位置一致。
struct RasterMeshData {
    std::vector<RasterVertex> vertices;
    std::vector<std::uint32_t> edges;
    glm::vec3 minimum{0};
    glm::vec3 maximum{0};
};

using RasterMeshProgress = std::function<void(std::size_t completed, std::size_t total)>;

// 输入须已通过网格数据校验。只执行 CPU 展开与边界计算，可在后台调用。
// 进度单位为位置和三角形；取消/失败不发布部分结果，不访问图形设备。
std::shared_ptr<const RasterMeshData> prepare_raster_mesh(const MeshData& mesh,
                                                          CancellationToken cancellation = {},
                                                          RasterMeshProgress progress = {});

// 界面线程拥有的 CPU 准备数据缓存。弱引用标识来源，保留当前场景和最近两份未使用数据。
// 生产线程只构造 RasterMeshData，完成后由界面线程供给缓存。
class RasterMeshCache {
  public:
    void supply(const std::shared_ptr<const MeshData>& mesh,
                std::shared_ptr<const RasterMeshData> prepared);
    std::shared_ptr<const RasterMeshData> find(const std::shared_ptr<const MeshData>& mesh);
    void retain(std::span<const RenderInstance> instances);

  private:
    struct Entry {
        std::shared_ptr<const RasterMeshData> data;
        std::uint64_t used = 0;
    };
    using Key = std::weak_ptr<const MeshData>;
    std::map<Key, Entry, std::owner_less<Key>> entries_;
    std::uint64_t clock_ = 0;
};

} // namespace minirender
