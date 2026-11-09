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
    void create(VulkanViewport& viewport);
    void draw(VulkanViewport& viewport, const minirender::GizmoLayout& layout,
              minirender::GizmoHandle highlighted);
    void release();

  private:
    struct Vertex {
        glm::vec2 position{0};
        glm::vec4 color{1};
    };
    void triangle(glm::dvec2 a, glm::dvec2 b, glm::dvec2 c, glm::vec4 color, glm::dvec2 size);
    void stroke(glm::dvec2 a, glm::dvec2 b, double width, glm::vec4 color, glm::dvec2 size);
    VkDevice device_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    GpuBuffer buffer_;
    std::vector<Vertex> vertices_;
};
