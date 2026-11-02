#pragma once

#include "gpu_buffer.hpp"
#include "viewport_renderer.hpp"
#include "vulkan_viewport.hpp"

#include <limits>

// 三角网格的实时直接光照管线。使用场景快照，UI 修改通过 revision 发布。
// A1.2 先实现基础 PBR 与法线视图；阴影和 HDR 离屏后处理在 A1.3 接入。
class SceneRenderer final : public ViewportRenderer {
  public:
    explicit SceneRenderer(VulkanViewport* window);
    void initResources() override;
    void initSwapChainResources() override;
    void releaseSwapChainResources() override;
    void releaseResources() override;
    void startNextFrame() override;

  private:
    void upload_scene();
    void update_uniforms();
    VulkanViewport* window_;
    VkDevice device_ = VK_NULL_HANDLE;
    GpuBuffer vertices_;
    GpuBuffer uniforms_;
    VkDescriptorSetLayout descriptor_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    std::uint32_t vertex_count_ = 0;
    std::uint64_t scene_revision_ = std::numeric_limits<std::uint64_t>::max();
};
