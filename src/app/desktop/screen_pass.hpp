#pragma once

#include "gpu_image.hpp"
#include "vulkan_viewport.hpp"

#include <span>

struct ScreenInput {
    VkImageView view;
    VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
};

// 单颜色附件的全屏 pass，供 AO、两个滤波方向和线性合成复用。
// binding 0 为帧 UBO，1..N 为最近邻输入；4 字节 push constant 传递滤波方向。
// 输出以 shader-read 布局结束，依赖关系覆盖上游写入和下游读取。
class ScreenPass {
  public:
    ~ScreenPass();
    void create(VulkanViewport& viewport, VkFormat format, const char* fragment, VkBuffer uniform,
                VkDeviceSize bytes, std::span<const ScreenInput> inputs);
    void draw(VulkanViewport& viewport, std::int32_t mode = 0);
    void release();
    VkImage image() const noexcept;
    VkImageView view() const noexcept;

  private:
    GpuImage target_;
    VkDevice device_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
    VkRenderPass pass_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};
