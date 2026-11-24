// 深度层按向上取整的半尺寸构建，奇数分辨率的边缘像素也会参与区间测试。
#include "screen_transport.hpp"

void ScreenTransport::create(VulkanViewport& viewport, const HdrTargets& targets, VkBuffer uniform,
                             VkDeviceSize bytes) {
    const auto        size = viewport.swapChainImageSize();
    VkExtent2D        extent{static_cast<std::uint32_t>(size.width()),
                             static_cast<std::uint32_t>(size.height())};
    const ScreenInput depth{targets.depth_view(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    ScreenInput       previous = depth;
    std::array<ScreenInput, 9> inputs{ScreenInput{targets.color_view()},
                                      ScreenInput{targets.normal_view()}, depth,
                                      ScreenInput{targets.albedo_view()}};
    for (std::size_t i = 0; i < levels_.size(); ++i) {
        extent = {(extent.width + 1) / 2, (extent.height + 1) / 2};
        levels_[i].create(viewport, VK_FORMAT_R32G32_SFLOAT, ":/shaders/depth_reduce.frag.spv",
                          uniform, bytes, {&previous, 1}, ":/shaders/screen.vert.spv", 3, extent);
        previous      = {levels_[i].view()};
        inputs[4 + i] = previous;
    }
    transport_.create(viewport, VK_FORMAT_R16G16B16A16_SFLOAT, ":/shaders/screen_trace.frag.spv",
                      uniform, bytes, inputs);
}

void ScreenTransport::draw(VulkanViewport& viewport, int mode) {
    const bool enabled = mode >= 13 && mode <= 18;
    if (!initialized_ || (enabled && viewport.screen_hiz())) {
        for (std::size_t i = 0; i < levels_.size(); ++i) {
            levels_[i].draw(viewport, i == 0 ? 1 : 0);
        }
    }
    if (!initialized_ || enabled) {
        transport_.draw(viewport);
    }
    initialized_ = true;
}

void ScreenTransport::release() {
    transport_.release();
    for (auto& level : levels_) {
        level.release();
    }
    initialized_ = false;
}
