#include "ssao_pass.hpp"

#include <array>

void SsaoPass::create(VulkanViewport& viewport, const HdrTargets& targets, VkBuffer uniform,
                      VkDeviceSize bytes) {
    const ScreenInput normal{targets.normal_view()};
    const ScreenInput depth{targets.depth_view(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    const std::array<ScreenInput, 2> ao_inputs{normal, depth};
    raw_.create(viewport, VK_FORMAT_R32_SFLOAT, ":/shaders/ssao.frag.spv", uniform, bytes,
                ao_inputs);
    const std::array<ScreenInput, 3> horizontal_inputs{ScreenInput{raw_.view()}, normal, depth};
    horizontal_.create(viewport, VK_FORMAT_R32_SFLOAT, ":/shaders/ao_filter.frag.spv", uniform,
                       bytes, horizontal_inputs);
    const std::array<ScreenInput, 3> vertical_inputs{ScreenInput{horizontal_.view()}, normal,
                                                     depth};
    filtered_.create(viewport, VK_FORMAT_R32_SFLOAT, ":/shaders/ao_filter.frag.spv", uniform, bytes,
                     vertical_inputs);
    const std::array<ScreenInput, 4> composite_inputs{
        ScreenInput{targets.color_view()}, ScreenInput{targets.ambient_view()},
        ScreenInput{filtered_.view()}, ScreenInput{raw_.view()}};
    composite_.create(viewport, VK_FORMAT_R32G32B32A32_SFLOAT, ":/shaders/ao_composite.frag.spv",
                      uniform, bytes, composite_inputs);
}
void SsaoPass::draw(VulkanViewport& viewport) {
    const bool enabled = viewport.ssao_settings().enabled;
    // 首帧建立全部描述符的有效布局；关闭时清成白色一次，后续跳过采样和滤波。
    if (enabled || was_enabled_ || !initialized_) {
        raw_.draw(viewport);
        horizontal_.draw(viewport, 0);
        filtered_.draw(viewport, 1);
        initialized_ = true;
    }
    composite_.draw(viewport);
    was_enabled_ = enabled;
}
void SsaoPass::release() {
    composite_.release();
    filtered_.release();
    horizontal_.release();
    raw_.release();
    initialized_ = was_enabled_ = false;
}
VkImage SsaoPass::hdr_image() const noexcept {
    return composite_.image();
}
VkImageView SsaoPass::hdr_view() const noexcept {
    return composite_.view();
}
VkImageView SsaoPass::raw_view() const noexcept {
    return raw_.view();
}
VkImageView SsaoPass::filtered_view() const noexcept {
    return filtered_.view();
}
