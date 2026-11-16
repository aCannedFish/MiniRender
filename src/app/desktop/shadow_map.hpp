// 方向光阴影图数组的分配、布局与逐层渲染。

#pragma once

#include "vulkan_viewport.hpp"

#include <array>
#include <vector>

// 一个最多八层的深度图数组，每层对应一个方向光。持有 image/view/sampler/framebuffer。
// 创建后 first_use 初始化全数组布局；每个 active layer 的 render pass 写入后恢复只读布局。
class ShadowMap {
  public:
    ShadowMap() = default;
    ~ShadowMap();
    ShadowMap(const ShadowMap&)               = delete;
    ShadowMap& operator=(const ShadowMap&)    = delete;
    static constexpr std::uint32_t resolution = 1024;
    static constexpr std::uint32_t layers     = 8;
    // 创建并绑定本对象所管理的 GPU 资源，失败时通过异常报告 Vulkan 操作。
    void create(VulkanViewport& viewport);
    // 释放独占资源并清空句柄；调用前必须结束 GPU 对这些资源的访问。
    void release();
    void prepare(VkCommandBuffer command);
    // 开始当前附件的 render pass，后续绘制由调用方录制。
    void begin(VkCommandBuffer command, std::uint32_t layer);
    // 返回主附件的 render pass，用于创建兼容的图形管线。
    VkRenderPass render_pass() const noexcept;
    // 返回采样器、图像视图与布局，供所属管线更新描述符。
    VkDescriptorImageInfo descriptor() const noexcept;

  private:
    // 借用的逻辑设备句柄，所有资源必须在设备销毁前释放。
    VkDevice device_ = VK_NULL_HANDLE;
    VkImage  image_  = VK_NULL_HANDLE;
    // 与本对象图像或缓冲绑定的独占显存。
    VkDeviceMemory memory_     = VK_NULL_HANDLE;
    VkImageView    array_view_ = VK_NULL_HANDLE;
    // 输入图像所使用的 Vulkan 采样器。
    VkSampler sampler_ = VK_NULL_HANDLE;
    // 本对象输出附件对应的 render pass。
    VkRenderPass                      pass_ = VK_NULL_HANDLE;
    std::array<VkImageView, layers>   views_{};
    std::array<VkFramebuffer, layers> framebuffers_{};
    bool                              initialized_ = false;
};
