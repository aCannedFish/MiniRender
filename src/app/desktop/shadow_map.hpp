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
    ShadowMap(const ShadowMap&) = delete;
    ShadowMap& operator=(const ShadowMap&) = delete;
    static constexpr std::uint32_t resolution = 1024;
    static constexpr std::uint32_t layers = 8;
    void create(VulkanViewport& viewport);
    void release();
    void prepare(VkCommandBuffer command);
    void begin(VkCommandBuffer command, std::uint32_t layer);
    VkRenderPass render_pass() const noexcept;
    VkDescriptorImageInfo descriptor() const noexcept;

  private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView array_view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkRenderPass pass_ = VK_NULL_HANDLE;
    std::array<VkImageView, layers> views_{};
    std::array<VkFramebuffer, layers> framebuffers_{};
    bool initialized_ = false;
};
