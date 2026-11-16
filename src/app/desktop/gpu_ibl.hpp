// 环境预处理图像的 GPU 上传、缓存和描述符输入。

#pragma once

#include "gpu_texture.hpp"
#include <minirender/render/ibl.hpp>

#include <array>
#include <span>

// 同尺寸浮点图层的上传资源。数组用于粗糙度，普通 2D 用于漫反射和 BRDF 表。
// 所有更新/回收由渲染线程在上一帧 fence 完成后执行。
class GpuFloatTexture {
  public:
    ~GpuFloatTexture();
    // 创建并绑定本对象所管理的 GPU 资源，失败时通过异常报告 Vulkan 操作。
    void create(VulkanViewport& viewport, std::span<const minirender::Image> layers);
    // 把尚未上传的图像数据复制命令录入当前 command buffer。
    void record_upload(VkCommandBuffer command);
    // 在上传提交的 fence 完成后回收暂存缓冲。
    void retire_upload();
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
    VkSampler                      sampler_ = VK_NULL_HANDLE;
    std::vector<VkBufferImageCopy> copies_;
    std::uint32_t                  layers_ = 0;
    // 是否已经录制上传，防止重复复制。
    bool uploaded_ = false;
};

// IBL 的四张逻辑纹理。始终提供有效 fallback 描述符，烘焙数据到达时分别替换。
// BRDF 独立于环境图更新，切换环境不重复上传同一张 BRDF 表。
class GpuIbl {
  public:
    // 创建并绑定本对象所管理的 GPU 资源，失败时通过异常报告 Vulkan 操作。
    void create(VulkanViewport& viewport);
    bool update(VulkanViewport& viewport, std::shared_ptr<const minirender::IblEnvironment> maps,
                std::shared_ptr<const minirender::Image> lut);
    // 把尚未上传的图像数据复制命令录入当前 command buffer。
    void record_upload(VkCommandBuffer command);
    // 释放独占资源并清空句柄；调用前必须结束 GPU 对这些资源的访问。
    void release();
    std::array<VkDescriptorImageInfo, 4> descriptors() const noexcept;

  private:
    void upload_environment(VulkanViewport& viewport);
    GpuFloatTexture                                   diffuse_, specular_, brdf_;
    GpuTexture                                        source_;
    std::shared_ptr<const minirender::IblEnvironment> maps_;
    std::shared_ptr<const minirender::Image>          lut_;
};
