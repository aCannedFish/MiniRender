// Vulkan 图像、显存和视图的独占生命周期。

#pragma once

#include <cstdint>
#include <vulkan/vulkan.h>

// 2D 图像或数组，独占 image / memory / view。布局转换由所属管线明确负责。
class GpuImage {
  public:
    GpuImage() = default;
    ~GpuImage();
    GpuImage(const GpuImage&)            = delete;
    GpuImage& operator=(const GpuImage&) = delete;
    // 创建并绑定本对象所管理的 GPU 资源，失败时通过异常报告 Vulkan 操作。
    void create(VkDevice device, VkPhysicalDevice physical, VkExtent2D extent, VkFormat format,
                VkImageUsageFlags usage, VkImageAspectFlags aspect, std::uint32_t mip_levels = 1,
                std::uint32_t array_layers = 1);
    // 释放独占资源并清空句柄；调用前必须结束 GPU 对这些资源的访问。
    void release();
    // 返回输出 VkImage 的借用句柄，用于后续命令或读回。
    VkImage image() const noexcept;
    // 返回输出图像视图的借用句柄，用于描述符绑定。
    VkImageView view() const noexcept;

  private:
    // 借用的逻辑设备句柄，所有资源必须在设备销毁前释放。
    VkDevice device_ = VK_NULL_HANDLE;
    VkImage  image_  = VK_NULL_HANDLE;
    // 与本对象图像或缓冲绑定的独占显存。
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView    view_   = VK_NULL_HANDLE;
};
