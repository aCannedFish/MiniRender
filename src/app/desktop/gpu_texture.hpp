// 纹理暂存上传、mip 层和采样器的 GPU 资源管理。

#pragma once

#include "gpu_buffer.hpp"
#include "gpu_image.hpp"
#include "vulkan_viewport.hpp"

// 上传 CPU 生成的线性 mip 链。shader 显式过滤，采样器不依赖 float32 线性过滤能力。
class GpuTexture {
  public:
    GpuTexture() = default;
    ~GpuTexture();
    GpuTexture(const GpuTexture&)            = delete;
    GpuTexture& operator=(const GpuTexture&) = delete;
    // 创建并绑定本对象所管理的 GPU 资源，失败时通过异常报告 Vulkan 操作。
    void create(VulkanViewport& viewport, const minirender::Texture& texture);
    // 把尚未上传的图像数据复制命令录入当前 command buffer。
    void record_upload(VkCommandBuffer command);
    // 在上传提交的 fence 完成后回收暂存缓冲。
    void retire_upload(); // 上一帧 fence 已完成后调用，回收 staging。
    // 释放独占资源并清空句柄；调用前必须结束 GPU 对这些资源的访问。
    void release();
    // 返回采样器、图像视图与布局，供所属管线更新描述符。
    VkDescriptorImageInfo descriptor() const noexcept;

  private:
    // 借用的逻辑设备句柄，所有资源必须在设备销毁前释放。
    VkDevice device_ = VK_NULL_HANDLE;
    GpuImage image_;
    // 等待复制到 GPU 图像的主机可见暂存数据。
    GpuBuffer staging_;
    // 输入图像所使用的 Vulkan 采样器。
    VkSampler sampler_ = VK_NULL_HANDLE;
    // 每个 mip 或数组层的复制范围。
    std::vector<VkBufferImageCopy> regions_;
    // 上传纹理包含的 mip 层数。
    std::uint32_t levels_ = 0;
    // 是否已经录制上传，防止重复复制。
    bool uploaded_ = false;
};
