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
    void initResources() override;
    void initSwapChainResources() override;
    void releaseSwapChainResources() override;
    void releaseResources() override;
    void startNextFrame() override;

  private:
    void upload(VkCommandBuffer command);
    VulkanViewport* window_;
    VkDevice device_ = VK_NULL_HANDLE;
    GpuImage texture_;
    GpuBuffer staging_;
    VkExtent2D extent_{};
    bool uploaded_ = false;
    std::shared_ptr<const minirender::Image> displayed_;
    std::vector<glm::vec4> pixels_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};
