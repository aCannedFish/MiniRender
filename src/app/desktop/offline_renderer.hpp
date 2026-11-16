// CPU 离线图像的 Vulkan 上传和视口显示。

#pragma once

#include "gpu_buffer.hpp"
#include "gpu_image.hpp"
#include "viewport_renderer.hpp"

#include <glm/vec4.hpp>
#include <memory>

class VulkanViewport;

// 显示 CPU 发布的线性图像。资源更新发生在上一帧 fence 完成后，复用同尺寸纹理和 staging。
// 只负责上传、曝光和等比例显示，不访问 RenderSession 或 Qt 控件。
class OfflineRenderer final : public ViewportRenderer {
  public:
    explicit OfflineRenderer(VulkanViewport* window);
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
    void upload(VkCommandBuffer command);
    // 借用的视口，提供设备、命令缓冲及当前场景状态。
    VulkanViewport* window_;
    // 借用的逻辑设备句柄，所有资源必须在设备销毁前释放。
    VkDevice device_ = VK_NULL_HANDLE;
    GpuImage texture_;
    // 等待复制到 GPU 图像的主机可见暂存数据。
    GpuBuffer staging_;
    // 当前附件尺寸，单位为物理像素。
    VkExtent2D extent_{};
    // 是否已经录制上传，防止重复复制。
    bool                                     uploaded_ = false;
    std::shared_ptr<const minirender::Image> displayed_;
    std::vector<glm::vec4>                   pixels_;
    // 输入图像所使用的 Vulkan 采样器。
    VkSampler sampler_ = VK_NULL_HANDLE;
    // 着色器资源的 binding 与类型声明。
    VkDescriptorSetLayout descriptor_layout_ = VK_NULL_HANDLE;
    // 供本管线分配描述符集合的池。
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    // 当前绑定的 uniform、纹理及采样器集合。
    VkDescriptorSet  descriptor_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_     = VK_NULL_HANDLE;
    // 当前绘制使用的 Vulkan 管线。
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};
