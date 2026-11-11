#pragma once

#include <minirender/geometry/sdf.hpp>

namespace minirender {

// 独立 SDF JSON 使用节点 ID 引用，文件顺序不限。读写后均校验图与参数。
// 写出通过同目录临时文件原子提交，取消或错误保留原目标。
SdfDocument load_sdf(const std::filesystem::path& path, CancellationToken cancellation = {});
void save_sdf(const SdfDocument& document, const std::filesystem::path& path,
              CancellationToken cancellation = {});

} // namespace minirender
