// 在同一帧的完整直接光缓存上处理 SSGI，时间累积完成后才发布下一帧可用的历史。
#include "temporal_transport.hpp"

void TemporalTransport::create(VulkanViewport& vp, const HdrTargets& targets, VkBuffer uniform,
                               VkDeviceSize bytes, VkImageView noisy) {
    const ScreenInput normal{targets.normal_view()},
        depth{targets.depth_view(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    ScreenInput source{noisy};
    for (auto& pass : spatial_) {
        const std::array inputs{source, normal, depth};
        pass.create(vp, VK_FORMAT_R32G32B32A32_SFLOAT, ":/shaders/gi_spatial.frag.spv", uniform,
                    bytes, inputs);
        source = {pass.view()};
    }
    for (auto& side : sides_) {
        const std::array guide_inputs{normal, depth, normal};
        side.guides.create(vp, VK_FORMAT_R32G32B32A32_SFLOAT, ":/shaders/gi_copy.frag.spv", uniform,
                           bytes, guide_inputs);
        const std::array history_inputs{source, source, source};
        side.history.create(vp, VK_FORMAT_R32G32B32A32_SFLOAT, ":/shaders/gi_copy.frag.spv",
                            uniform, bytes, history_inputs);
        const std::array temporal_inputs{source, ScreenInput{targets.ambient_view()},
                                         ScreenInput{side.history.view()},
                                         ScreenInput{side.guides.view()}};
        side.filtered.create(vp, VK_FORMAT_R32G32B32A32_SFLOAT, ":/shaders/gi_temporal.frag.spv",
                             uniform, bytes, temporal_inputs);
        side.history.set_image(1, {side.filtered.view()});
    }
}

void TemporalTransport::draw(VulkanViewport& vp, std::size_t index) {
    for (std::size_t i = 0; i < spatial_.size(); ++i) {
        spatial_[i].draw(vp, 1 << i);
    }
    auto& side = sides_.at(index);
    side.filtered.draw(vp);
    side.history.draw(vp);
    side.guides.draw(vp, 3);
}

void TemporalTransport::initialize(VulkanViewport& vp) {
    // 静态描述符中的两侧图像在首次比较前均进入可采样布局。
    if (initialized_) {
        return;
    }
    for (auto& side : sides_) {
        side.guides.clear(vp);
        side.history.clear(vp);
        side.filtered.clear(vp);
    }
    initialized_ = true;
}

void TemporalTransport::release() {
    for (auto& side : sides_) {
        side.filtered.release();
        side.history.release();
        side.guides.release();
    }
    initialized_ = false;
    for (auto& pass : spatial_) {
        pass.release();
    }
}
