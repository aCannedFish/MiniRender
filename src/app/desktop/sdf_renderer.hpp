// 距离场图元与 CSG 的 Vulkan 步进显示。

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
    // 标记下一次渲染需要把线性图像复制到主机读回缓冲。
    void request_hdr_capture() override;
    // 在提交完成后读取线性图像；调用方负责先等待 GPU 同步。
    minirender::Image read_hdr_capture() override;

  private:
    void update_frame();
    void create_display();
    void display();
    void capture();
    VulkanViewport* viewport_;
    // 借用的逻辑设备句柄，所有资源必须在设备销毁前释放。
    VkDevice  device_ = VK_NULL_HANDLE;
    GpuBuffer uniform_;
    // 保存 GPU 图像复制结果的主机可见缓冲。
    GpuBuffer  readback_;
    ScreenPass trace_;
    // 是否已经录制上传，防止重复复制。
    std::shared_ptr<const minirender::SdfDocument> uploaded_;
    std::unique_ptr<minirender::SdfField>          field_;
    // 输入图像所使用的 Vulkan 采样器。
    VkSampler             sampler_     = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptors_ = VK_NULL_HANDLE;
    // 本 pass 的描述符池。
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    // 当前绑定的 uniform、纹理及采样器集合。
    VkDescriptorSet  descriptor_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_     = VK_NULL_HANDLE;
    // 当前绘制使用的 Vulkan 管线。
    VkPipeline pipeline_          = VK_NULL_HANDLE;
    bool       capture_requested_ = false;
    // 是否已有可读取的图像复制结果。
    bool captured_ = false;
};
