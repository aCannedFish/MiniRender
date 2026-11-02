#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>

// 小型动态数据的 host-visible/coherent 缓冲。当前一帧在途，调用方先等 fence 再写入。
// 显存由本类独占，移动后旧对象为空；release 必须在所属 device 销毁前调用。
class GpuBuffer {
  public:
    GpuBuffer() = default;
    ~GpuBuffer();
    GpuBuffer(const GpuBuffer&) = delete;
    GpuBuffer& operator=(const GpuBuffer&) = delete;
    GpuBuffer(GpuBuffer&& other) noexcept;
    GpuBuffer& operator=(GpuBuffer&& other) noexcept;
    void create(VkDevice device, VkPhysicalDevice physical, VkDeviceSize bytes,
                VkBufferUsageFlags usage);
    void write(const void* data, std::size_t bytes);
    void release();
    VkBuffer handle() const noexcept;

  private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkDeviceSize size_ = 0;
};

void check_vk(VkResult result, const char* operation);
