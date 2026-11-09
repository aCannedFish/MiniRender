#pragma once

#include <minirender/geometry/point_cloud.hpp>

namespace minirender {

enum class PlyFormat { ascii, little_endian, big_endian };

// 文件与标量预算同时覆盖被跳过的属性及元素，避免长 list 绕过点数限制。
struct PlyReadOptions {
    std::size_t maximum_points = 2000000;
    std::size_t maximum_records = 8000000;
    std::size_t maximum_values = 64000000;
    std::size_t maximum_file_bytes = 2ULL * 1024 * 1024 * 1024;
    std::size_t maximum_header_bytes = 1024 * 1024;
};

// 按 PLY 1.0 header 读取 vertex 位置及可选 nx/ny/nz，其他属性/元素完整读取后跳过。
// 已有 face 连接不进入点云。进度单位为所有元素记录，取消检查也覆盖长 list。
PointCloud load_point_cloud(const std::filesystem::path& path, PlyReadOptions options = {},
                            CancellationToken cancellation = {}, PointProgress progress = {});

// 写位置与可选法线，默认采用紧凑的 binary little-endian。
// 临时文件成功写出后再原子替换目标；进度单位为点，失败或取消保留原文件。
void save_point_cloud(const PointCloud& cloud, const std::filesystem::path& path,
                      PlyFormat format = PlyFormat::little_endian,
                      CancellationToken cancellation = {}, PointProgress progress = {});

} // namespace minirender
