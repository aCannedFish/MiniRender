// GAMES202 HW5 与自建帧序列使用相同的四张引导 EXR 和矩阵记录。
#pragma once
#include <minirender/render/denoiser.hpp>

namespace minirender {
// 同一帧的四张几何引导图和矩阵记录路径，按 HW5 约定成组读取。
struct GuideFiles {
    std::filesystem::path normal, position, depth, object, matrix;
};

// 构造 HW5 已发布的逐帧文件名，路径支持非 ASCII 字符。
GuideFiles guide_files(const std::filesystem::path& directory, std::size_t frame);
DenoiseGuides read_guides(const GuideFiles& files, CancellationToken cancellation = {});
void write_guides(const DenoiseGuides& guides, const GuideFiles& files);
} // namespace minirender
