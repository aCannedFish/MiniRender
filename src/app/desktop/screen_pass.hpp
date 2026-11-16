// 可复用的全屏 Vulkan 渲染附件、描述符与命令录制。

#pragma once

#include "gpu_image.hpp"
#include "vulkan_viewport.hpp"

#include <span>

struct ScreenInput {
    VkImageView   view;
    VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
};

// 单颜色附件的全屏 pass，供 AO、两个滤波方向和线性合成复用。
// binding 0 为帧 UBO，1..N 为最近邻输入；4 字节 push constant 传递滤波方向。
// 输出以 shader-read 布局结束，依赖关系覆盖上游写入和下游读取。
class ScreenPass {
  public:
    ~ScreenPass();
    // 创建并绑定本对象所管理的 GPU 资源，失败时通过异常报告 Vulkan 操作。
    void create(VulkanViewport& viewport, VkFormat format, const char* fragment, VkBuffer uniform,
                VkDeviceSize bytes, std::span<const ScreenInput> inputs,
                const char* vertex = ":/shaders/screen.vert.spv", std::uint32_t vertices = 3);
    // 录制该 pass 的绘制命令，输出图像交由后续 pass 或显示阶段使用。
    void draw(VulkanViewport& viewport, std::int32_t mode = 0);
    // 释放独占资源并清空句柄；调用前必须结束 GPU 对这些资源的访问。
    void release();
    // 返回输出 VkImage 的借用句柄，用于后续命令或读回。
    VkImage image() const noexcept;
    // 返回输出图像视图的借用句柄，用于描述符绑定。
    VkImageView view() const noexcept;

  private:
    GpuImage target_;
    // 借用的逻辑设备句柄，所有资源必须在设备销毁前释放。
    VkDevice device_ = VK_NULL_HANDLE;
    // 当前附件尺寸，单位为物理像素。
    VkExtent2D extent_{};
    // 本对象输出附件对应的 render pass。
    VkRenderPass pass_ = VK_NULL_HANDLE;
    // 当前尺寸的附件绑定。
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
    // 输入图像所使用的 Vulkan 采样器。
    VkSampler sampler_ = VK_NULL_HANDLE;
    // 着色器资源的 binding 与类型声明。
    VkDescriptorSetLayout descriptor_layout_ = VK_NULL_HANDLE;
    // 本 pass 的描述符池。
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    // 当前绑定的 uniform、纹理及采样器集合。
    VkDescriptorSet  descriptor_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_     = VK_NULL_HANDLE;
    // 当前绘制使用的 Vulkan 管线。
    VkPipeline    pipeline_ = VK_NULL_HANDLE;
    std::uint32_t vertices_ = 3;
};
