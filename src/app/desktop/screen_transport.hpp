// 共用光栅附件生成五级深度区间，录制镜面探针和一次漫反射屏幕空间传输。
#pragma once
#include "hdr_targets.hpp"
#include "screen_pass.hpp"
#include "temporal_transport.hpp"
#include <array>

// 持有随视口尺寸建立的离屏通道，显示模块通过 view() 读取合成输入。
class ScreenTransport {
  public:
    // 绑定直接光、法线、深度和线性底色，分配各级深度区间纹理。
    void create(VulkanViewport& viewport, const HdrTargets& targets, VkBuffer uniform,
                VkDeviceSize bytes);
    // 首帧建立有效采样布局，单纯的逐像素遍历可跳过后续帧的深度归约。
    void draw(VulkanViewport& viewport, int mode, bool reference);
    // 首次选择时间滤波时建立历史附件，必须在本帧绘制之前调用。
    void prepare_temporal(VulkanViewport& viewport);
    // 交换链销毁或改变尺寸时释放渲染附件和描述符。
    void release();

    VkImageView view() const noexcept {
        return resolved_.view();
    }

  private:
    // 五个逐级减半的深度区间，源深度图充当第零级。
    std::array<ScreenPass, 5> levels_;
    // 本帧屏幕射线的原始输出，包括反射、命中调试或单次间接光。
    ScreenPass transport_;
    // 发布给 HDR 合成的全尺寸图像，显示源可选原始或累计结果。
    ScreenPass        resolved_;
    TemporalTransport temporal_;
    // 借用几何附件与帧常量，为延迟创建的历史通道提供输入。
    const HdrTargets* targets_ = nullptr;
    VkBuffer          uniform_{};
    VkDeviceSize      bytes_ = 0;
    // 分别判断历史附件是否已分配、是否已转到可采样布局。
    bool temporal_ready_ = false, temporal_initialized_ = false;
    bool initialized_ = false;
};
