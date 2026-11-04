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
    void create(VulkanViewport& viewport, std::span<const minirender::Image> layers);
    void record_upload(VkCommandBuffer command);
    void retire_upload();
    void release();
    VkDescriptorImageInfo descriptor() const noexcept;

  private:
    VkDevice device_ = VK_NULL_HANDLE;
    GpuImage image_;
    GpuBuffer staging_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    std::vector<VkBufferImageCopy> copies_;
    std::uint32_t layers_ = 0;
    bool uploaded_ = false;
};

// IBL 的四张逻辑纹理。始终提供有效 fallback 描述符，烘焙数据到达时分别替换。
// BRDF 独立于环境图更新，切换环境不重复上传同一张 BRDF 表。
class GpuIbl {
  public:
    void create(VulkanViewport& viewport);
    bool update(VulkanViewport& viewport, std::shared_ptr<const minirender::IblEnvironment> maps,
                std::shared_ptr<const minirender::Image> lut);
    void record_upload(VkCommandBuffer command);
    void release();
    std::array<VkDescriptorImageInfo, 4> descriptors() const noexcept;

  private:
    void upload_environment(VulkanViewport& viewport);
    GpuFloatTexture diffuse_, specular_, brdf_;
    GpuTexture source_;
    std::shared_ptr<const minirender::IblEnvironment> maps_;
    std::shared_ptr<const minirender::Image> lut_;
};
