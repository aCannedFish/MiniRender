#pragma once

#include "gpu_buffer.hpp"
#include "gpu_image.hpp"
#include "vulkan_viewport.hpp"

// 上传 CPU 生成的线性 mip 链。shader 显式过滤，采样器不依赖 float32 线性过滤能力。
class GpuTexture {
  public:
    GpuTexture() = default;
    ~GpuTexture();
    GpuTexture(const GpuTexture&) = delete;
    GpuTexture& operator=(const GpuTexture&) = delete;
    void create(VulkanViewport& viewport, const minirender::Texture& texture);
    void record_upload(VkCommandBuffer command);
    void retire_upload(); // 上一帧 fence 已完成后调用，回收 staging。
    void release();
    VkDescriptorImageInfo descriptor() const noexcept;

  private:
    VkDevice device_ = VK_NULL_HANDLE;
    GpuImage image_;
    GpuBuffer staging_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    std::vector<VkBufferImageCopy> regions_;
    std::uint32_t levels_ = 0;
    bool uploaded_ = false;
};
