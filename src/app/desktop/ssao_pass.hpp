// 屏幕空间遮蔽、双边滤波和环境漫反射合成的 Vulkan pass。

#pragma once

#include "hdr_targets.hpp"
#include "screen_pass.hpp"
#include "screen_transport.hpp"

// SSAO 的原始遮蔽、水平/垂直双边滤波及线性 HDR 合成。
// 背景为 1，只有独立的环境漫反射附件被遮蔽；HDR 导出读取合成后的图像。
class SsaoPass {
  public:
    // 创建并绑定本对象所管理的 GPU 资源，失败时通过异常报告 Vulkan 操作。
    void create(VulkanViewport& viewport, const HdrTargets& targets, VkBuffer uniform,
                VkDeviceSize bytes, VkImageView reference);
    // 录制该 pass 的绘制命令，输出图像交由后续 pass 或显示阶段使用。
    void draw(VulkanViewport& viewport, int mode, bool reference);

    void prepare_temporal(VulkanViewport& vp) {
        transport_.prepare_temporal(vp);
    }

    // 释放独占资源并清空句柄；调用前必须结束 GPU 对这些资源的访问。
    void set_reference(VkImageView view) {
        composite_.set_image(6, {view});
    }

    void release();
    // 返回线性合成图像，作为实时 HDR 导出的数据来源。
    VkImage hdr_image() const noexcept;
    // 返回线性合成结果的视图，供显示变换采样。
    VkImageView hdr_view() const noexcept;
    // 返回未滤波的遮蔽附件视图。
    VkImageView raw_view() const noexcept;
    // 返回双边滤波后的遮蔽附件视图。
    VkImageView filtered_view() const noexcept;

  private:
    ScreenTransport transport_;
    ScreenPass      raw_, horizontal_, filtered_, composite_;
    bool            initialized_ = false;
    bool            was_enabled_ = false;
};
