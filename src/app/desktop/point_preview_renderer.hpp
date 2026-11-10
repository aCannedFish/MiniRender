#pragma once

#include "gpu_buffer.hpp"
#include "viewport_renderer.hpp"
#include <minirender/render/point_preview.hpp>

class VulkanViewport;

// 独立点云诊断视图：实例化圆片写深度，抽样法线和边界读取该深度。
// GPU 缓冲只在数据所有权改变后更新，参数改变仅写 push constants。
class PointPreviewRenderer final : public ViewportRenderer {
  public:
    explicit PointPreviewRenderer(VulkanViewport* viewport);
    void initResources() override;
    void initSwapChainResources() override;
    void releaseSwapChainResources() override;
    void releaseResources() override;
    void startNextFrame() override;
    // 调用方已经等过帧 fence；清空工具数据后释放上一份预览缓冲。
    void discard_if_unused();

  private:
    VkPipeline create_pipeline(bool lines, bool instanced);
    void upload();
    VulkanViewport* viewport_;
    VkDevice device_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline points_pipeline_ = VK_NULL_HANDLE;
    VkPipeline normals_pipeline_ = VK_NULL_HANDLE;
    VkPipeline boundary_pipeline_ = VK_NULL_HANDLE;
    GpuBuffer points_;
    GpuBuffer normals_;
    GpuBuffer boundary_;
    std::shared_ptr<const minirender::PointPreviewData> uploaded_;
};
