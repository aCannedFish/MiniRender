// 点云、法线与边界的独立 Vulkan 预览。

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
    // 调用方已经等过帧 fence；清空工具数据后释放上一份预览缓冲。
    void discard_if_unused();

  private:
    // 按 pass 类型、剔除方式和正面绕序创建 Vulkan 图形管线。
    VkPipeline create_pipeline(bool lines, bool instanced);
    void upload();
    VulkanViewport* viewport_;
    // 借用的逻辑设备句柄，所有资源必须在设备销毁前释放。
    VkDevice         device_            = VK_NULL_HANDLE;
    VkPipelineLayout layout_            = VK_NULL_HANDLE;
    VkPipeline       points_pipeline_   = VK_NULL_HANDLE;
    VkPipeline       normals_pipeline_  = VK_NULL_HANDLE;
    VkPipeline       boundary_pipeline_ = VK_NULL_HANDLE;
    GpuBuffer        points_;
    GpuBuffer        normals_;
    GpuBuffer        boundary_;
    // 是否已经录制上传，防止重复复制。
    std::shared_ptr<const minirender::PointPreviewData> uploaded_;
};
