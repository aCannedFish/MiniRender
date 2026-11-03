#pragma once

#include <cstdint>
#include <vulkan/vulkan.h>

// 单层离屏附件，独占 image / memory / view。布局转换由所属 render pass 明确负责。
class GpuImage {
  public:
    GpuImage() = default;
    ~GpuImage();
    GpuImage(const GpuImage&) = delete;
    GpuImage& operator=(const GpuImage&) = delete;
    void create(VkDevice device, VkPhysicalDevice physical, VkExtent2D extent, VkFormat format,
                VkImageUsageFlags usage, VkImageAspectFlags aspect, std::uint32_t mip_levels = 1);
    void release();
    VkImage image() const noexcept;
    VkImageView view() const noexcept;

  private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
};
