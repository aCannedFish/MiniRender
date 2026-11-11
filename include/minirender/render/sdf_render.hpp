#pragma once

#include <minirender/core/image.hpp>
#include <minirender/geometry/sdf.hpp>

#include <functional>

namespace minirender {

enum class SdfView { surface, normals, depth, steps };

struct SdfRenderOptions {
    std::size_t width = 128;
    std::size_t height = 128;
    SdfView view = SdfView::surface;
    bool shadows = true;
};

// 图像使用线性 RGB。统计分别记录主射线命中、步数耗尽和未定义梯度。
struct SdfRenderResult {
    Image image;
    std::size_t hits = 0;
    std::size_t limited = 0;
    std::size_t steps = 0;
    std::size_t normal_fallbacks = 0;
};

using SdfProgress = std::function<void(std::size_t completed, std::size_t total)>;

// 确定性的单线程针孔参照，用于核对 Vulkan 图像。每像素采样中心，支持取消和进度。
// 阴影沿方向光反向步进；达到阴影步数上限时按遮挡处理。
SdfRenderResult render_sdf(const SdfDocument& document, SdfRenderOptions options = {},
                           CancellationToken cancellation = {}, SdfProgress progress = {});

// 步数从蓝、青、黄过渡到红；耗尽上限的射线在渲染器中单独显示为品红。
glm::vec3 sdf_step_color(std::uint32_t steps, std::uint32_t budget);

} // namespace minirender
