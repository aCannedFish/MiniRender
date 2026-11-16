// 可编辑网格邻接及局部拓扑修改，供重网格算法使用。

#pragma once

#include <minirender/geometry/remesh.hpp>

#include <optional>
#include <unordered_map>

namespace minirender::detail {

using RemeshEdge = std::uint64_t;
// 把无向边端点排序并编码为稳定的键。
RemeshEdge remesh_edge(MeshIndex a, MeshIndex b) noexcept;
// 从无向边键恢复两个端点索引。
std::pair<MeshIndex, MeshIndex> remesh_endpoints(RemeshEdge edge) noexcept;

// 工作顶点只属于几何 worker。faces 保持有效邻接，version 用于丢弃队列中的旧候选。
struct RemeshVertex {
    // 可变工作顶点的局部空间坐标。
    glm::dvec3 position{0};
    // 工作顶点当前参数化位置。
    glm::vec2 uv{0};
    // 该顶点引用的有效邻接面索引。
    std::vector<MeshIndex> faces;
    MeshIndex              component = invalid_mesh_index; // 按保护边划分的原表面片区。
    // 每次局部修改后递增，供候选队列拒绝旧记录。
    std::uint64_t version = 0;
    // 元素是否仍参与当前网格。
    bool alive = true;
    // 顶点是否位于当前网格边界。
    bool boundary = false;
    // 顶点位置是否受保护，不能自由平滑移动。
    bool fixed = false;
    // UV 或其他属性约束是否禁止自由修改。
    bool attribute_locked = false;
    // 顶点是否位于硬法线接缝。
    bool normal_seam = false;
};

// 面在一轮中保留编号，component 指向固定的原表面查询片区。
struct RemeshFace {
    // 工作面保存的三个面角及属性。
    MeshTriangle triangle;
    MeshIndex    component = 0;
    // 元素是否仍参与当前网格。
    bool alive = true;
};

// affected 包含邻接受影响的顶点，调用方据此更新局部边候选。
struct RemeshEdit {
    // 本次局部修改是否实际成功。
    bool applied = false;
    // 需要重新评估候选的邻接顶点集合。
    std::vector<MeshIndex> affected;
};

// 定向三角网格的可变工作副本。面编号在一轮中稳定，邻接随局部操作更新。
// 保护边在分裂时变成两条子边；折叠与翻边保持这张特征图。
// 每轮结束后 compact 清理失效元素并重编号，所有旧队列须先销毁。
class RemeshMesh {
  public:
    // 从不可变输入复制工作状态，并根据拓扑和保护设置建立特征图。
    RemeshMesh(const MeshData& input, const HalfedgeMesh& topology, const RemeshSettings& settings,
               CancellationToken cancellation);
    // 返回工作顶点的只读视图，其中失效元素由 alive 标记。
    std::span<const RemeshVertex> vertices() const noexcept;
    // 返回工作面的只读视图，面编号在 compact 前保持稳定。
    std::span<const RemeshFace> faces() const noexcept;
    // 返回当前存活三角形数量。
    std::size_t triangle_count() const noexcept;
    // 返回按保护边分割出的原表面片区数。
    MeshIndex component_count() const noexcept;
    std::vector<RemeshEdge> edges() const;
    // 收集顶点的有效一环邻点。
    std::vector<MeshIndex> neighbors(MeshIndex vertex) const;
    // 返回同时使用两个端点的有效邻接面。
    std::vector<MeshIndex> edge_faces(MeshIndex a, MeshIndex b) const;
    // 检查两个工作顶点之间是否存在有效边。
    bool has_edge(MeshIndex a, MeshIndex b) const;
    // 检查边是否属于边界、属性接缝或受保护折痕。
    bool protected_edge(RemeshEdge edge) const;
    // 计算当前工作位置对应的三角形方向。
    glm::dvec3 normal(const MeshTriangle& triangle) const;
    // 按邻接有效面计算顶点的局部方向。
    glm::dvec3 vertex_normal(MeshIndex vertex) const;
    // 在指定位置分裂边及邻接面，传播特征并报告受影响的一环。
    RemeshEdit split(RemeshEdge edge, glm::dvec3 position, std::optional<glm::vec2> uv = {});
    // 检查拓扑、方向和边长限制后折叠边，失败时保留工作网格。
    RemeshEdit collapse(RemeshEdge edge, glm::dvec3 position, double maximum_edge,
                        std::optional<glm::vec2> uv = {});
    // 在合法的两面邻域内翻转对角线，保持约束和有效面。
    RemeshEdit flip(RemeshEdge edge);
    // 检查候选位置和 UV 是否会导致退化或局部翻转。
    bool valid_positions(std::span<const glm::dvec3> positions,
                         std::span<const glm::vec2>  uv) const;
    // 应用通过检查的位置和 UV，返回实际更新数量。
    std::size_t relocate(std::span<const glm::dvec3> positions, std::span<const glm::vec2> uv);
    // 清理失效顶点与面并重建索引；调用方须放弃之前的候选队列。
    void compact();
    // 生成独立的不可变网格，按参数决定是否重算平滑法线。
    std::shared_ptr<const MeshData> publish(bool update_normals) const;

  private:
    // 工作顶点当前参数化位置。
    enum Feature : std::uint8_t { boundary = 1, uv = 2, normal_jump = 4, crease = 8 };

    // 仅替换候选顶点位置，计算该三角形的候选方向。
    glm::dvec3 normal_with(const MeshTriangle& triangle, MeshIndex vertex,
                           glm::dvec3 position) const;
    // 检查候选面面积和相对原方向，拒绝退化与翻转。
    bool valid_face(const MeshTriangle& candidate, glm::dvec3 before, MeshIndex moved_vertex,
                    glm::dvec3 position) const;
    // 比较修改前后的 UV 绕序，保护参数化方向。
    bool uv_orientation(const MeshTriangle& before, const MeshTriangle& after) const;
    // 检查边折叠的一环连接条件，并收集局部受影响顶点。
    bool link_condition(MeshIndex a, MeshIndex b, std::vector<MeshIndex>& local) const;
    // 检查指定有效面是否引用给定顶点。
    bool contains(MeshIndex face, MeshIndex vertex) const;
    // 替换面角索引并同步工作顶点的邻接面列表。
    void replace_face(MeshIndex face, const MeshTriangle& triangle);
    // 将面标为失效并从邻接关系中移除。
    void remove_face(MeshIndex face);
    // 添加属于指定原表面片区的新面并登记邻接。
    void add_face(const MeshTriangle& triangle, MeshIndex component);
    // 更新受影响顶点版本，使旧的局部候选失效。
    void changed(std::vector<MeshIndex>& vertices);
    // 包含存活和待压缩元素的工作顶点数组。
    std::vector<RemeshVertex> vertices_;
    // 在一轮处理中保持编号稳定的工作面数组。
    std::vector<RemeshFace> faces_;
    // 边键到保护类型位标记的映射。
    std::unordered_map<RemeshEdge, std::uint8_t> features_;
    // 本次重网格固定使用的保护条件和阈值。
    RemeshSettings settings_;
    // 局部操作和邻域扫描中检查的取消标记。
    CancellationToken token_;
    // 当前有效面数，与失效标记同步维护。
    std::size_t triangle_count_ = 0;
    // 用于原表面投影的保护片区数量。
    MeshIndex component_count_ = 0;
};

} // namespace minirender::detail
