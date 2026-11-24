// 着色器为 SSGI 做空间与时间滤波。左右两种显示配置分别保存颜色、法线与深度历史。
#pragma once
#include "hdr_targets.hpp"
#include "screen_pass.hpp"
#include <array>

class TemporalTransport {
  public:
    void create(VulkanViewport& viewport, const HdrTargets& targets, VkBuffer uniform,
                VkDeviceSize bytes, VkImageView noisy);
    void initialize(VulkanViewport& viewport);
    void draw(VulkanViewport& viewport, std::size_t side);

    VkImageView view(std::size_t side) const noexcept {
        return sides_[side].filtered.view();
    }

    void release();

  private:
    struct Side {
        ScreenPass filtered, history, guides;
        bool       initialized = false;
    };

    std::array<ScreenPass, 3> spatial_;
    std::array<Side, 2>       sides_;
};
