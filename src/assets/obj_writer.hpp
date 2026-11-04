#pragma once

#include <minirender/assets/scene_io.hpp>
#include <ostream>

namespace minirender::detail {

// 内部流写出器，供单文件导出和生成资产保存复用。调用方负责校验输入、关闭及提交。
void write_obj_content(const MeshData& mesh, std::ostream& stream, CancellationToken cancellation,
                       const MeshWriteProgress& progress);

} // namespace minirender::detail
