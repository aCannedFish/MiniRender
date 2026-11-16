// 实时颜色、法线、深度和线框附件及浮点图像读回。

#pragma once

#include "gpu_buffer.hpp"
#include "gpu_image.hpp"
#include "vulkan_viewport.hpp"

#include <minirender/core/image.hpp>

// 主场景的线性 HDR、环境漫反射、法线和深度附件。尺寸跟随交换链。
// HDR 为 RGBA32F，不依赖设备对 float32 线性过滤的可选支持。
// 线框写入独立 UNORM 附件，读取主场景深度，保留 HDR 读回的原始辐射值。
class HdrTargets {
  public:
    HdrTargets() = default;
    ~HdrTargets();
    HdrTargets(const HdrTargets&)            = delete;
    HdrTargets& operator=(const HdrTargets&) = delete;
    // 创建并绑定本对象所管理的 GPU 资源，失败时通过异常报告 Vulkan 操作。
    void create(VulkanViewport& viewport);
    // 释放独占资源并清空句柄；调用前必须结束 GPU 对这些资源的访问。
    void release();
    // 开始当前附件的 render pass，后续绘制由调用方录制。
    void begin(VkCommandBuffer command);
    // 调用方先完成主场景 pass。此 pass 复用已有深度，仅写线框颜色。
    // 使用已有深度开启线框 pass，仅写入线框颜色。
    void begin_wire(VkCommandBuffer command);
    // 查询线框图像是否已完成首次布局初始化。
    bool wire_initialized() const noexcept;
    // 返回线框 render pass，用于创建兼容的管线。
    VkRenderPass wire_pass() const noexcept;
    // 返回线框图像视图，供最终显示采样。
    VkImageView wire_view() const noexcept;
    // 返回主附件的 render pass，用于创建兼容的图形管线。
    VkRenderPass render_pass() const noexcept;
    // 返回主场景线性颜色附件的采样视图。
    VkImageView color_view() const noexcept;
    // 返回环境漫反射附件，供 SSAO 独立合成。
    VkImageView ambient_view() const noexcept;
    // 返回表面法线附件，供 SSAO、轮廓和诊断显示使用。
    VkImageView normal_view() const noexcept;
    // 返回主场景深度附件的只读视图。
    VkImageView depth_view() const noexcept;
    // source 为空时读原始光照，否则读同尺寸 RGBA32F 合成结果。
    // 录制图像到主机缓冲的复制；这里不等待命令执行完成。
    void record_readback(VkCommandBuffer command, VkImage source = VK_NULL_HANDLE);
    // 将已完成的 GPU 复制结果解码为 CPU 线性 RGB 图像。
    minirender::Image readback(); // 调用方先等当前提交 fence。

  private:
    // 借用的逻辑设备句柄，所有资源必须在设备销毁前释放。
    VkDevice device_ = VK_NULL_HANDLE;
    // 借用的物理设备，用于查询内存和硬件能力。
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    // 当前附件尺寸，单位为物理像素。
    VkExtent2D extent_{};
    // 主场景线性 HDR 颜色附件。
    GpuImage color_;
    // 单独保存的环境漫反射附件。
    GpuImage ambient_;
    // 世界空间表面法线与几何有效标记。
    GpuImage normal_;
    // 主场景深度附件。
    GpuImage depth_;
    // 保留遮挡关系的线框显示附件。
    GpuImage wire_;
    // 线框颜色 pass，复用已有场景深度。
    VkRenderPass wire_pass_ = VK_NULL_HANDLE;
    // 线框颜色与场景深度的附件组合。
    VkFramebuffer wire_framebuffer_ = VK_NULL_HANDLE;
    // 线框附件是否已经取得可采样布局。
    bool wire_initialized_ = false;
    // 保存 GPU 图像复制结果的主机可见缓冲。
    GpuBuffer readback_;
    // 本对象输出附件对应的 render pass。
    VkRenderPass pass_ = VK_NULL_HANDLE;
    // 当前尺寸的附件绑定。
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
    // 是否已有可读取的图像复制结果。
    bool captured_ = false;
};
