#pragma once

#include "vulkan_viewport.hpp"

// A0 的真实 GPU 诊断管线：上传棋盘纹理、两层三角形、深度测试与回读。
// VulkanViewport 拥有 device/swapchain；此类拥有 texture/descriptor/pipeline/staging，
// 并在 VulkanViewport 指定的释放阶段销毁。后续场景管线复用这些资源生命周期约定。
class DiagnosticRenderer final {
  public:
    explicit DiagnosticRenderer(VulkanViewport* window);
    void initResources();
    void initSwapChainResources();
    void releaseSwapChainResources();
    void releaseResources();
    void startNextFrame();

  private:
    void create_texture();
    void create_descriptors();
    void create_pipeline();
    void record_texture_upload(VkCommandBuffer command);
    std::uint32_t memory_type(std::uint32_t bits, VkMemoryPropertyFlags flags) const;

    VulkanViewport* window_;
    VkDevice device_ = VK_NULL_HANDLE;
    VkImage texture_ = VK_NULL_HANDLE;
    VkDeviceMemory texture_memory_ = VK_NULL_HANDLE;
    VkImageView texture_view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkBuffer staging_ = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    bool uploaded_ = false;
};
