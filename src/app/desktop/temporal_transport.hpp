// 着色器为 SSGI 做空间与时间滤波。左右两种显示配置分别保存颜色、法线与深度历史。
#pragma once
#include "hdr_targets.hpp"
#include "screen_pass.hpp"
#include <array>

// 同一相机下共用空间滤波附件，两侧时间历史各自更新。
class TemporalTransport {
  public:
    // 建立三层空间通道及两套时间历史，连接它们的图像输入。
    void create(VulkanViewport& viewport, const HdrTargets& targets, VkBuffer uniform,
                VkDeviceSize bytes, VkImageView noisy);
    // 首次采样前清空历史并建立有效的 Vulkan 布局。
    void initialize(VulkanViewport& viewport);
    // 先进行空间滤波，再向指定对照侧发布积累颜色和本帧引导。
    void draw(VulkanViewport& viewport, std::size_t side);

    VkImageView view(std::size_t side) const noexcept {
        return sides_[side].filtered.view();
    }

    // 释放与交换链尺寸相关的管线和图像。
    void release();

  private:
    // 一侧的输出、前帧累积和前帧法线/深度保存在独立附件中。
    struct Side {
        ScreenPass filtered, history, guides;
        bool       initialized = false;
    };

    // 按一、二、四个纹素的步长滤波。
    std::array<ScreenPass, 3> spatial_;
    std::array<Side, 2>       sides_;
};
