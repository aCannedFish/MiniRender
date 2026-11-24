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
    const ScreenInput raw{transport_.view()};
    const std::array  source{raw, raw, raw};
    resolved_.create(viewport, VK_FORMAT_R16G16B16A16_SFLOAT, ":/shaders/gi_copy.frag.spv", uniform,
                     bytes, source);
    targets_ = &targets;
    uniform_ = uniform;
    bytes_   = bytes;
}

void ScreenTransport::prepare_temporal(VulkanViewport& viewport) {
    if (temporal_ready_) {
        return;
    }
    temporal_.create(viewport, *targets_, uniform_, bytes_, transport_.view());
    resolved_.set_image(2, {temporal_.view(0)});
    resolved_.set_image(3, {temporal_.view(1)});
    temporal_ready_ = true;
}

void ScreenTransport::draw(VulkanViewport& viewport, int mode, bool reference) {
    const bool enabled = mode >= 13 && mode <= 20;
    if (!initialized_ || (enabled && viewport.screen_hiz())) {
        for (std::size_t i = 0; i < levels_.size(); ++i) {
            levels_[i].draw(viewport, i == 0 ? 1 : 0);
        }
    }
    if (!initialized_ || enabled) {
        transport_.draw(viewport);
    }
    if (temporal_ready_ && !temporal_initialized_ && mode < 19) {
        // 分屏先绘制普通一侧时，绑定的新历史附件也必须经过布局转换。
        temporal_.initialize(viewport);
        temporal_initialized_ = true;
    }
    if (mode >= 19) {
        temporal_.draw(viewport, reference ? 1U : 0U);
        temporal_initialized_ = true;
    }
    if (!initialized_ || enabled) {
        resolved_.draw(viewport, mode >= 19 ? (reference ? 2 : 1) : 0);
    }
    initialized_ = true;
}

void ScreenTransport::release() {
    resolved_.release();
    temporal_.release();
    temporal_ready_ = temporal_initialized_ = false;
    transport_.release();
    for (auto& level : levels_) {
        level.release();
    }
    initialized_ = false;
}
