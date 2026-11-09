#pragma once

#include <minirender/geometry/remesh.hpp>

#include <optional>
#include <unordered_map>

namespace minirender::detail {

using RemeshEdge = std::uint64_t;
RemeshEdge remesh_edge(MeshIndex a, MeshIndex b) noexcept;
std::pair<MeshIndex, MeshIndex> remesh_endpoints(RemeshEdge edge) noexcept;

// 工作顶点只属于几何 worker。faces 保持有效邻接，version 用于丢弃队列中的旧候选。
struct RemeshVertex {
    glm::dvec3 position{0};
    glm::vec2 uv{0};
    std::vector<MeshIndex> faces;
    MeshIndex component = invalid_mesh_index; // 按保护边划分的原表面片区。
    std::uint64_t version = 0;
    bool alive = true;
    bool boundary = false;
    bool fixed = false;
    bool attribute_locked = false;
    bool normal_seam = false;
};
// 面在一轮中保留编号，component 指向固定的原表面查询片区。
struct RemeshFace {
    MeshTriangle triangle;
    MeshIndex component = 0;
    bool alive = true;
};
// affected 包含邻接受影响的顶点，调用方据此更新局部边候选。
struct RemeshEdit {
    bool applied = false;
    std::vector<MeshIndex> affected;
};

// 定向三角网格的可变工作副本。面编号在一轮中稳定，邻接随局部操作更新。
// 保护边在分裂时变成两条子边；折叠与翻边保持这张特征图。
// 每轮结束后 compact 清理失效元素并重编号，所有旧队列须先销毁。
class RemeshMesh {
  public:
    RemeshMesh(const MeshData& input, const HalfedgeMesh& topology, const RemeshSettings& settings,
               CancellationToken cancellation);
    std::span<const RemeshVertex> vertices() const noexcept;
    std::span<const RemeshFace> faces() const noexcept;
    std::size_t triangle_count() const noexcept;
    MeshIndex component_count() const noexcept;
    std::vector<RemeshEdge> edges() const;
    std::vector<MeshIndex> neighbors(MeshIndex vertex) const;
    std::vector<MeshIndex> edge_faces(MeshIndex a, MeshIndex b) const;
    bool has_edge(MeshIndex a, MeshIndex b) const;
    bool protected_edge(RemeshEdge edge) const;
    glm::dvec3 normal(const MeshTriangle& triangle) const;
    glm::dvec3 vertex_normal(MeshIndex vertex) const;
    RemeshEdit split(RemeshEdge edge, glm::dvec3 position, std::optional<glm::vec2> uv = {});
    RemeshEdit collapse(RemeshEdge edge, glm::dvec3 position, double maximum_edge,
                        std::optional<glm::vec2> uv = {});
    RemeshEdit flip(RemeshEdge edge);
    bool valid_positions(std::span<const glm::dvec3> positions,
                         std::span<const glm::vec2> uv) const;
    std::size_t relocate(std::span<const glm::dvec3> positions, std::span<const glm::vec2> uv);
    void compact();
    std::shared_ptr<const MeshData> publish(bool update_normals) const;

  private:
    enum Feature : std::uint8_t { boundary = 1, uv = 2, normal_jump = 4, crease = 8 };
    glm::dvec3 normal_with(const MeshTriangle& triangle, MeshIndex vertex,
                           glm::dvec3 position) const;
    bool valid_face(const MeshTriangle& candidate, glm::dvec3 before, MeshIndex moved_vertex,
                    glm::dvec3 position) const;
    bool uv_orientation(const MeshTriangle& before, const MeshTriangle& after) const;
    bool link_condition(MeshIndex a, MeshIndex b, std::vector<MeshIndex>& local) const;
    bool contains(MeshIndex face, MeshIndex vertex) const;
    void replace_face(MeshIndex face, const MeshTriangle& triangle);
    void remove_face(MeshIndex face);
    void add_face(const MeshTriangle& triangle, MeshIndex component);
    void changed(std::vector<MeshIndex>& vertices);
    std::vector<RemeshVertex> vertices_;
    std::vector<RemeshFace> faces_;
    std::unordered_map<RemeshEdge, std::uint8_t> features_;
    RemeshSettings settings_;
    CancellationToken token_;
    std::size_t triangle_count_ = 0;
    MeshIndex component_count_ = 0;
};

} // namespace minirender::detail
