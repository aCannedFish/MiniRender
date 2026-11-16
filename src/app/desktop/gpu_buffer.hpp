// Vulkan 缓冲区与内存的独占生命周期及主机读写。

#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>

// 小型动态数据的 host-visible/coherent 缓冲。当前一帧在途，调用方先等 fence 再写入。
// 显存由本类独占，移动后旧对象为空；release 必须在所属 device 销毁前调用。
class GpuBuffer {
  public:
    // 建立空缓冲；移动转移显存所有权，复制被禁用。
    GpuBuffer() = default;
    // 销毁独占的缓冲和显存；调用方须保证设备与同步仍然有效。
    ~GpuBuffer();
    GpuBuffer(const GpuBuffer&)            = delete;
    GpuBuffer& operator=(const GpuBuffer&) = delete;
    // 建立空缓冲；移动转移显存所有权，复制被禁用。
    GpuBuffer(GpuBuffer&& other) noexcept;
    GpuBuffer& operator=(GpuBuffer&& other) noexcept;
    // 分配 host-visible/coherent 显存并绑定缓冲；调用方给出用途与字节容量。
    void create(VkDevice device, VkPhysicalDevice physical, VkDeviceSize bytes,
                VkBufferUsageFlags usage);
    // 把指定字节数复制到映射显存；调用前必须保证 GPU 已结束访问。
    void write(const void* data, std::size_t bytes);
    // 从映射显存复制指定字节数；GPU 写入与同步由调用方先完成。
    void read(void* data, std::size_t bytes); // GPU 写入完成后调用。
    // 释放缓冲和显存并清空句柄；所属设备必须仍有效。
    void release();
    // 返回借用的 VkBuffer 句柄，所有权仍属于本对象。
    VkBuffer handle() const noexcept;

  private:
    // 用于分配和销毁资源的借用设备句柄。
    VkDevice device_ = VK_NULL_HANDLE;
    // 独占的 Vulkan 缓冲句柄。
    VkBuffer buffer_ = VK_NULL_HANDLE;
    // 绑定到缓冲的独占显存。
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    // 已分配的字节容量，读写时用于边界检查。
    VkDeviceSize size_ = 0;
};

// 将非成功 Vulkan 返回值转换为带操作名称的异常。
void check_vk(VkResult result, const char* operation);
