#pragma once

#include "gpu_buffer.hpp"
#include "gpu_image.hpp"
#include "vulkan_viewport.hpp"

#include <minirender/core/image.hpp>

// 主场景的线性 HDR、法线和深度附件。尺寸跟随交换链，但与最终显示图像分开。
// HDR 为 RGBA32F，不依赖设备对 float32 线性过滤的可选支持。
class HdrTargets {
  public:
    HdrTargets() = default;
    ~HdrTargets();
    HdrTargets(const HdrTargets&) = delete;
    HdrTargets& operator=(const HdrTargets&) = delete;
    void create(VulkanViewport& viewport);
    void release();
    void begin(VkCommandBuffer command);
    VkRenderPass render_pass() const noexcept;
    VkImageView color_view() const noexcept;
    VkImageView normal_view() const noexcept;
    VkImageView depth_view() const noexcept;
    void record_readback(VkCommandBuffer command);
    minirender::Image readback(); // 调用方先等当前提交 fence。

  private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
    GpuImage color_;
    GpuImage normal_;
    GpuImage depth_;
    GpuBuffer readback_;
    VkRenderPass pass_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
    bool captured_ = false;
};
