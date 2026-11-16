// 基础纹理、深度和读回的 Vulkan 诊断渲染。

#pragma once

#include "viewport_renderer.hpp"
#include "vulkan_viewport.hpp"

// A0 的真实 GPU 诊断管线：上传棋盘纹理、两层三角形、深度测试与回读。
// VulkanViewport 拥有 device/swapchain；此类拥有 texture/descriptor/pipeline/staging，
// 并在 VulkanViewport 指定的释放阶段销毁。后续场景管线复用这些资源生命周期约定。
class DiagnosticRenderer final : public ViewportRenderer {
  public:
    explicit DiagnosticRenderer(VulkanViewport* window);
    // 在设备建立后创建与窗口尺寸无关的 GPU 资源。
    void initResources() override;
    // 按当前视口尺寸创建附件和相关管线，窗口缩放后重新调用。
    void initSwapChainResources() override;
    // 销毁尺寸相关附件和管线；调用前 GPU 必须已结束使用。
    void releaseSwapChainResources() override;
    // 释放设备级资源，必须早于所属 Vulkan 设备销毁。
    void releaseResources() override;
    // 读取已发布状态并录制本帧命令，在帧完成后通知视口。
    void startNextFrame() override;

  private:
    void create_texture();
    // 建立本管线使用的描述符布局、池和绑定。
    void create_descriptors();
    // 按 pass 类型、剔除方式和正面绕序创建 Vulkan 图形管线。
    void create_pipeline();
    void record_texture_upload(VkCommandBuffer command);
    std::uint32_t memory_type(std::uint32_t bits, VkMemoryPropertyFlags flags) const;

    // 借用的视口，提供设备、命令缓冲及当前场景状态。
    VulkanViewport* window_;
    // 借用的逻辑设备句柄，所有资源必须在设备销毁前释放。
    VkDevice       device_         = VK_NULL_HANDLE;
    VkImage        texture_        = VK_NULL_HANDLE;
    VkDeviceMemory texture_memory_ = VK_NULL_HANDLE;
    VkImageView    texture_view_   = VK_NULL_HANDLE;
    // 输入图像所使用的 Vulkan 采样器。
    VkSampler sampler_ = VK_NULL_HANDLE;
    // 等待复制到 GPU 图像的主机可见暂存数据。
    VkBuffer       staging_        = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory_ = VK_NULL_HANDLE;
    // 着色器资源的 binding 与类型声明。
    VkDescriptorSetLayout descriptor_layout_ = VK_NULL_HANDLE;
    // 供本管线分配描述符集合的池。
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    // 当前绑定的 uniform、纹理及采样器集合。
    VkDescriptorSet descriptor_ = VK_NULL_HANDLE;
    // 描述符布局与 push constant 范围的组合。
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    // 当前绘制使用的 Vulkan 管线。
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    // 是否已经录制上传，防止重复复制。
    bool uploaded_ = false;
};
