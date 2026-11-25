// 只读半边拓扑、边界环、连通分量与流形校验。

#pragma once

#include <minirender/core/cancellation.hpp>
#include <minirender/scene/scene.hpp>

#include <limits>
#include <span>

namespace minirender {

using MeshIndex                               = std::uint32_t;
inline constexpr MeshIndex invalid_mesh_index = std::numeric_limits<MeshIndex>::max();

// halfedge 指向该顶点的一条出边。边界顶点优先指向边界半边；孤立位置为 invalid。
// valence 计数所有相邻位置，包含边界环上的两个邻点。
struct HalfedgeVertex {
    MeshIndex halfedge = invalid_mesh_index;
    MeshIndex valence  = 0;
};

// origin 是起点，twin 指向反向半边，next/previous 沿同一面的绕序连接。
// 每条边总有两个半边。边界半边的 face 为 invalid，next/previous 沿边界环连接。
struct Halfedge {
    MeshIndex origin   = invalid_mesh_index;
    MeshIndex twin     = invalid_mesh_index;
    MeshIndex next     = invalid_mesh_index;
    MeshIndex previous = invalid_mesh_index;
    MeshIndex face     = invalid_mesh_index;
    MeshIndex edge     = invalid_mesh_index;
};

// 每条无向边保存一条有面的代表半边，另一侧通过 twin 访问。
struct HalfedgeEdge {
    MeshIndex halfedge = invalid_mesh_index; // 始终选取有面的半边。
};

// 从代表半边沿 next 遍历该面的有向边界。
struct HalfedgeFace {
    MeshIndex halfedge = invalid_mesh_index;
};

// Euler 数只计被面引用的位置。孤立位置保留在 MeshData 中，单独报告。
struct TopologyStatistics {
    std::size_t  referenced_vertices  = 0;
    std::size_t  isolated_vertices    = 0;
    std::size_t  edges                = 0;
    std::size_t  faces                = 0;
    std::size_t  boundary_edges       = 0;
    std::size_t  boundary_loops       = 0;
    std::size_t  face_components      = 0;
    std::int64_t euler_characteristic = 0;
};

// 有向流形三角网格的只读拓扑，不持有输入引用或修改 MeshData。
// 按位置索引连接，不焊接坐标相同的独立位置，UV/法线接缝不影响连接。
// 顶点/面索引与 MeshData 相同，面角 c 的半边为 3*face+c；边界半边追加在其后。
// 边按首次遇到的面角次序编号，哈希表只查找、不决定输出顺序。
// 构造时检查数据、边的面数与方向、重复面、边界环及顶点的单一面片扇。
// 非法输入抛 invalid_argument，索引容量不足抛 length_error，取消抛 OperationCancelled。
class HalfedgeMesh {
  public:
    // 按位置索引配对半边，补齐边界环，并在构造中检查绕序与流形约束。
    explicit HalfedgeMesh(const MeshData& mesh, CancellationToken cancellation = {});
    // 借用顶点表，顶点编号与输入位置索引对应。
    std::span<const HalfedgeVertex> vertices() const noexcept;
    // 借用有向半边表，可沿 twin 或 next 遍历邻接关系。
    std::span<const Halfedge> halfedges() const noexcept;
    // 借用无向边表，每条边通过代表半边访问两侧。
    std::span<const HalfedgeEdge> edges() const noexcept;
    // 借用面表，从代表半边出发沿 next 可取得三条边。
    std::span<const HalfedgeFace> faces() const noexcept;
    // 返回构造时统计的边界、分量和 Euler 特征数。
    const TopologyStatistics& statistics() const noexcept;

    // 参数须为本对象中的有效半边索引。绕顶点遍历可使用 next(twin(h))。
    MeshIndex destination(MeshIndex halfedge) const noexcept;
    // 判断该有向半边所在一侧是否为外边界。
    bool is_boundary(MeshIndex halfedge) const noexcept;

  private:
    std::vector<HalfedgeVertex> vertices_;
    std::vector<Halfedge>       halfedges_;
    std::vector<HalfedgeEdge>   edges_;
    std::vector<HalfedgeFace>   faces_;
    TopologyStatistics          statistics_;
};

} // namespace minirender
