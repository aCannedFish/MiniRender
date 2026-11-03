#pragma once

#include "gpu_buffer.hpp"
#include "gpu_image.hpp"
#include "vulkan_viewport.hpp"

#include <minirender/core/image.hpp>

// 主场景的线性 HDR、法线和深度附件。尺寸跟随交换链，但与最终显示图像分开。
// HDR 为 RGBA32F，不依赖设备对 float32 线性过滤的可选支持。
// 线框写入独立 UNORM 附件，读取主场景深度，保留 HDR 读回的原始辐射值。
class HdrTargets {
  public:
    HdrTargets() = default;
    ~HdrTargets();
    HdrTargets(const HdrTargets&) = delete;
    HdrTargets& operator=(const HdrTargets&) = delete;
    void create(VulkanViewport& viewport);
    void release();
    void begin(VkCommandBuffer command);
    // 调用方先完成主场景 pass。此 pass 复用已有深度，仅写线框颜色。
    void begin_wire(VkCommandBuffer command);
    bool wire_initialized() const noexcept;
    VkRenderPass wire_pass() const noexcept;
    VkImageView wire_view() const noexcept;
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
    GpuImage wire_;
    VkRenderPass wire_pass_ = VK_NULL_HANDLE;
    VkFramebuffer wire_framebuffer_ = VK_NULL_HANDLE;
    bool wire_initialized_ = false;
    GpuBuffer readback_;
    VkRenderPass pass_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
    bool captured_ = false;
};
