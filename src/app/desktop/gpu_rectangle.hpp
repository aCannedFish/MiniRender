// 矩形灯的世界空间四角、辐亮度和 LTC 纹理。资源随场景版本更新。
#pragma once
#include "gpu_ibl.hpp"
#include <minirender/render/rectangle_light.hpp>

// 持有设备侧 LTC 查找表与矩形灯实例缓冲，描述符借用这些资源。
class GpuRectangleLights {
  public:
    // 创建共用的拟合表，灯具实例缓冲按首次场景上传的实际数量分配。
    void create(VulkanViewport& viewport);
    // 上一帧结束后更新灯列表，返回是否需要重绑缓冲描述符。
    bool update(VulkanViewport& viewport, const std::vector<minirender::RectangleLight>& lights);
    void record_upload(VkCommandBuffer command);
    void release();
    std::array<VkDescriptorImageInfo, 2> tables() const;
    VkDescriptorBufferInfo buffer() const;

  private:
    std::array<GpuFloatTexture, 2> textures_;
    GpuBuffer                      lights_;
    std::size_t                    capacity_ = 0;
};
