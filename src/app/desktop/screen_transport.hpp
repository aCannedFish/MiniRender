// 共用光栅附件生成五级深度区间，录制镜面探针和一次漫反射屏幕空间传输。
#pragma once
#include "hdr_targets.hpp"
#include "screen_pass.hpp"
#include "temporal_transport.hpp"
#include <array>

class ScreenTransport {
  public:
    void create(VulkanViewport& viewport, const HdrTargets& targets, VkBuffer uniform,
                VkDeviceSize bytes);
    // 首帧建立有效采样布局，单纯的逐像素遍历可跳过后续帧的深度归约。
    void draw(VulkanViewport& viewport, int mode, bool reference);
    void prepare_temporal(VulkanViewport& viewport);
    void release();

    VkImageView view() const noexcept {
        return resolved_.view();
    }

  private:
    std::array<ScreenPass, 5> levels_;
    ScreenPass                transport_;
    ScreenPass                resolved_;
    TemporalTransport         temporal_;
    const HdrTargets*         targets_ = nullptr;
    VkBuffer                  uniform_{};
    VkDeviceSize              bytes_          = 0;
    bool                      temporal_ready_ = false, temporal_initialized_ = false;
    bool                      initialized_ = false;
};
