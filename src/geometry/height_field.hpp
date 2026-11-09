#pragma once

#include <minirender/geometry/displacement.hpp>

namespace minirender::detail {

// 均匀位移与自适应位移共用双精度 Repeat 双线性高度及完整接缝检查。
struct HeightFieldSamples {
    std::vector<double> offsets;
    std::size_t sampled_corners = 0;
    std::size_t seam_edges = 0;
    std::size_t seam_segments = 0;
    double maximum_seam_difference = 0;
};
double height_value(const Image& image, glm::dvec2 uv);
HeightFieldSamples inspect_height_field(const MeshData& input, const HalfedgeMesh& topology,
                                        const Image& image, const DisplacementSettings& settings,
                                        CancellationToken cancellation,
                                        DisplacementProgressCallback progress = {});

} // namespace minirender::detail
