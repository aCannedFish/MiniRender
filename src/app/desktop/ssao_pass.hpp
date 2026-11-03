#pragma once

#include "hdr_targets.hpp"
#include "screen_pass.hpp"

// SSAO 的原始遮蔽、水平/垂直双边滤波及线性 HDR 合成。
// 背景为 1，只有独立的环境漫反射附件被遮蔽；HDR 导出读取合成后的图像。
class SsaoPass {
  public:
    void create(VulkanViewport& viewport, const HdrTargets& targets, VkBuffer uniform,
                VkDeviceSize bytes);
    void draw(VulkanViewport& viewport);
    void release();
    VkImage hdr_image() const noexcept;
    VkImageView hdr_view() const noexcept;
    VkImageView raw_view() const noexcept;
    VkImageView filtered_view() const noexcept;

  private:
    ScreenPass raw_, horizontal_, filtered_, composite_;
    bool initialized_ = false;
    bool was_enabled_ = false;
};
