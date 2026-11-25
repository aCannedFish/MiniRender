// 移动轴与旋转环的 GPU 显示及交互高亮。

#pragma once

#include "gpu_buffer.hpp"
#include <minirender/scene/gizmo.hpp>

class VulkanViewport;

// 将逻辑像素中的操纵器线段展开为彩色三角形，在交换链后处理 renderpass 中绘制。
// 覆盖层使用固定像素线宽、关闭深度测试，不写入离线结果或 HDR 附件。
// 缓冲在帧 fence 完成后写入；资源随交换链创建和释放。
class GizmoRenderer {
  public:
    ~GizmoRenderer();
    // 创建并绑定本对象所管理的 GPU 资源，失败时通过异常报告 Vulkan 操作。
    void create(VulkanViewport& viewport);
    // 录制该 pass 的绘制命令，输出图像交由后续 pass 或显示阶段使用。
    void draw(VulkanViewport& viewport, const minirender::GizmoLayout& layout,
              minirender::GizmoHandle highlighted);
    // 释放独占资源并清空句柄；调用前必须结束 GPU 对这些资源的访问。
    void release();

  private:
    // 操纵器覆盖层的二维顶点位置与 RGBA 颜色。
    struct Vertex {
        glm::vec2 position{0};
        glm::vec4 color{1};
    };

    void triangle(glm::dvec2 a, glm::dvec2 b, glm::dvec2 c, glm::vec4 color, glm::dvec2 size);
    void stroke(glm::dvec2 a, glm::dvec2 b, double width, glm::vec4 color, glm::dvec2 size);
    // 借用的逻辑设备句柄，所有资源必须在设备销毁前释放。
    VkDevice         device_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    // 当前绘制使用的 Vulkan 管线。
    VkPipeline          pipeline_ = VK_NULL_HANDLE;
    GpuBuffer           buffer_;
    std::vector<Vertex> vertices_;
};
