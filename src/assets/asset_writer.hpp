#pragma once

#include <minirender/assets/scene_io.hpp>

namespace minirender::detail {

// 已有资产尽量重定向相对路径；跨卷时复制到 assets，内容相同则复用，冲突则换名。
std::filesystem::path portable_asset_path(const std::filesystem::path& source,
                                          const std::filesystem::path& destination,
                                          std::size_t index, std::size_t count,
                                          CancellationToken cancellation,
                                          const SceneSaveCallback& progress);

// 生成网格先写入本次独占暂存目录，再按内容命名发布到 assets；已有不同内容永不覆盖。
std::filesystem::path write_generated_asset(const MeshData& mesh,
                                            const std::filesystem::path& destination,
                                            const std::filesystem::path& staging, std::size_t index,
                                            std::size_t count, CancellationToken cancellation,
                                            const SceneSaveCallback& progress);

} // namespace minirender::detail
