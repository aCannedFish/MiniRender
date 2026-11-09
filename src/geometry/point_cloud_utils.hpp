#pragma once

#include <minirender/geometry/point_cloud.hpp>

namespace minirender::detail {

// 精确重复位置合并为首条记录，保留其输入索引及可选法线。+0/-0 视为相同位置。
struct UniquePointCloud {
    PointCloud cloud;
    std::vector<std::uint32_t> source_indices;
};
UniquePointCloud unique_points(const PointCloud& input, CancellationToken cancellation,
                               PointProgress progress = {});

} // namespace minirender::detail
