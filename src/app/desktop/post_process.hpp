#pragma once

#include "hdr_targets.hpp"

// 独立显示 pass：从线性 HDR、法线、深度生成 swapchain 图像。
// 浮点纹理用最近邻采样器；FXAA 的双线性插值在感知颜色空间显式实现。
class PostProcess {
  public:
    PostProcess() = default;
    ~PostProcess();
    PostProcess(const PostProcess&) = delete;
    PostProcess& operator=(const PostProcess&) = delete;
    void create(VulkanViewport& viewport, const HdrTargets& targets, VkBuffer uniform_buffer,
                VkDeviceSize uniform_size, VkImageView hdr, VkImageView raw_ao,
                VkImageView filtered_ao);
    void draw(VulkanViewport& viewport);
    void release();

  private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};
