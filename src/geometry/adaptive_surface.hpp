#pragma once

#include "loop_hierarchy.hpp"
#include <minirender/geometry/adaptive.hpp>

namespace minirender::detail {

// barycentric 为原控制三角形中的参数，几何 ID 在所有根面间共享，UV 按根面独立保留。
struct AdaptiveCorner {
    MeshIndex point;
    glm::dvec3 barycentric;
};
struct AdaptiveFace {
    std::array<AdaptiveCorner, 3> corners;
    MeshIndex root = 0;
    std::uint32_t level = 0;
};
struct AdaptiveSample {
    glm::dvec3 position;
    glm::dvec3 normal;
};

// 按需采样有限层均匀 Loop/位移表面，误差查询只展开命中的参数三角形及支持一环。
// 位移对同一几何点使用首次高度，随后各根面检查接缝一致性。
class AdaptiveSurface final {
  public:
    AdaptiveSurface(std::shared_ptr<const MeshData> input, const HalfedgeMesh& topology,
                    std::shared_ptr<const Texture> height, const AdaptiveSettings& settings,
                    CancellationToken cancellation);
    AdaptiveFace root_face(MeshIndex root) const;
    std::array<AdaptiveFace, 4> children(const AdaptiveFace& parent);
    AdaptiveSample sample(MeshIndex root, glm::dvec3 barycentric);
    glm::dvec3 position(const AdaptiveCorner& corner, MeshIndex root);
    glm::vec2 uv(MeshIndex root, glm::dvec3 barycentric) const;
    void visit_height_features(const AdaptiveFace& face, double threshold,
                               const std::function<void(glm::dvec3)>& visit);
    LoopHierarchy& hierarchy() noexcept;
    std::size_t queries() const noexcept;
    double maximum_displacement() const noexcept;
    double maximum_seam_difference() const noexcept;

  private:
    glm::dvec2 parameter_uv(MeshIndex root, glm::dvec3 barycentric) const;
    std::shared_ptr<const MeshData> input_;
    std::shared_ptr<const Texture> height_;
    AdaptiveSettings settings_;
    CancellationToken cancellation_;
    LoopHierarchy hierarchy_;
    std::unordered_map<MeshIndex, glm::dvec3> positions_;
    std::unordered_map<MeshIndex, glm::dvec3> normals_;
    std::size_t queries_ = 0;
    std::size_t texture_tests_ = 0;
    double maximum_displacement_ = 0;
    double maximum_seam_difference_ = 0;
};

} // namespace minirender::detail
