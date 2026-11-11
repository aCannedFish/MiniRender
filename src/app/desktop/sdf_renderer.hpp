#pragma once

#include "gpu_buffer.hpp"
#include "screen_pass.hpp"
#include "viewport_renderer.hpp"
#include <minirender/render/sdf_render.hpp>

// 独立 SDF 管线：两个全屏三角形写入线性 RGBA32F，再显示或读回。
// 与三角场景共享设备及帧同步，图元和相机数据在上一帧 fence 完成后更新。
class SdfRenderer final : public ViewportRenderer {
  public:
    explicit SdfRenderer(VulkanViewport* viewport);
    void initResources() override;
    void initSwapChainResources() override;
    void releaseSwapChainResources() override;
    void releaseResources() override;
    void startNextFrame() override;
    void request_hdr_capture() override;
    minirender::Image read_hdr_capture() override;

  private:
    void update_frame();
    void create_display();
    void display();
    void capture();
    VulkanViewport* viewport_;
    VkDevice device_ = VK_NULL_HANDLE;
    GpuBuffer uniform_;
    GpuBuffer readback_;
    ScreenPass trace_;
    std::shared_ptr<const minirender::SdfDocument> uploaded_;
    std::unique_ptr<minirender::SdfField> field_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptors_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    bool capture_requested_ = false;
    bool captured_ = false;
};
