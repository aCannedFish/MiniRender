// 三角网格实时渲染、资源缓存、阴影与显示管线连接。

#pragma once

#include "gizmo_renderer.hpp"
#include "gpu_buffer.hpp"
#include "gpu_ibl.hpp"
#include "gpu_rectangle.hpp"
#include "gpu_subsurface.hpp"
#include "gpu_texture.hpp"
#include "hdr_targets.hpp"
#include "post_process.hpp"
#include "shadow_map.hpp"
#include "ssao_pass.hpp"
#include "viewport_renderer.hpp"
#include "vulkan_viewport.hpp"

#include <limits>
#include <map>

// 三角网格的实时表面光照管线。使用场景快照，UI 修改通过 revision 发布。
// 分开记录环境漫反射，经 SSAO 合成为线性 HDR，再执行显示变换。
class SceneRenderer final : public ViewportRenderer {
  public:
    explicit SceneRenderer(VulkanViewport* window);
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
    enum class Pass { surface, shadow, wire, background };
    // 按 pass 类型、剔除方式和正面绕序创建 Vulkan 图形管线。
    VkPipeline create_pipeline(Pass pass, VkCullModeFlags cull = VK_CULL_MODE_NONE,
                               VkFrontFace front = VK_FRONT_FACE_COUNTER_CLOCKWISE);
    // 按显示模式初始化或绘制线框附件，沿用场景深度遮挡。
    void render_wire(VkCommandBuffer command, int mode);
    // 逐个方向光录制阴影深度绘制。
    void render_shadow_maps(VkCommandBuffer command, int mode);
    // 根据快照版本更新实例参数，复用仍有效的网格和纹理资源。
    void upload_scene();
    // 在纹理绑定或实例资源变化时重建描述符关联。
    void rebuild_texture_descriptors();
    // 将已完成的环境预处理结果绑定到实时光照管线。
    void update_ibl_descriptors();
    using MeshHandle    = std::shared_ptr<const minirender::MeshData>;
    using TextureHandle = std::shared_ptr<const minirender::Texture>;

    // 网格保留局部坐标。变换与材质写入独立 SSBO，不再烘焙到每个顶点。
    struct Mesh {
        GpuBuffer     vertices;
        GpuBuffer     edges;
        std::uint32_t count = 0;
        glm::vec3     minimum{0};
        glm::vec3     maximum{0};
    };

    struct Draw {
        MeshHandle      mesh;
        TextureHandle   texture;
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
    };

    // 按共享网格所有权索引的 GPU 网格缓存。
    std::map<MeshHandle, Mesh, std::owner_less<MeshHandle>> meshes_;
    // 按场景实例顺序记录的网格和纹理绑定。
    std::vector<Draw> draws_;
    // 次表面材质的实时表面近似缓存。
    SubsurfacePreviewCache subsurface_previews_;
    // 无底色纹理实例共用的白色纹理。
    TextureHandle white_texture_;
    std::map<TextureHandle, std::unique_ptr<GpuTexture>, std::owner_less<TextureHandle>> textures_;
    // 把相机、显示、光源及效果设置打包为本帧 GPU uniform。
    void update_uniforms(int mode, bool reference);
    // 录制一个显示模式的几何、阴影与屏幕空间处理，结果保留在线性附件中。
    void render_view(int mode, bool reference);
    // 使用场景与视口参数版本及显示模式判断每侧图像是否需要重绘。
    std::array<std::pair<std::uint64_t, int>, 2> comparison_keys_{};
    // 借用的视口，提供设备、命令缓冲及当前场景状态。
    VulkanViewport* window_;
    // 借用的逻辑设备句柄，所有资源必须在设备销毁前释放。
    VkDevice device_ = VK_NULL_HANDLE;
    // 方向光阴影图数组与逐层绘制资源。
    ShadowMap shadow_map_;
    // 主场景颜色、法线、深度与线框附件。
    HdrTargets targets_;
    // 环境遮蔽采样、滤波和合成阶段。
    SsaoPass ssao_;
    // 线性图像到屏幕编码的后处理阶段。
    PostProcess post_process_;
    // 物体操纵器的叠加绘制。
    GizmoRenderer gizmo_;
    // 按场景几何尺度计算的原色接触遮蔽半径，单位为米。
    float preview_radius_ = 0.2F;
    // 下一次绘制是否需要录制线性图像读回。
    bool                     shadow_depth_ready_ = false;
    bool                     capture_hdr_        = false;
    std::array<glm::mat4, 8> shadow_matrices_{};
    // 每个场景实例的变换与材质数据缓冲。
    GpuBuffer instances_;
    // 实例 SSBO 的当前容量，扩容时才重新分配。
    std::size_t instance_capacity_ = 0;
    // 本帧相机、光源与效果设置的统一缓冲。
    GpuBuffer uniforms_;
    // 当前环境预处理结果对应的 GPU 数据。
    GpuIbl             ibl_;
    GpuRectangleLights rectangles_;
    // 着色器资源的 binding 与类型声明。
    VkDescriptorSetLayout descriptor_layout_ = VK_NULL_HANDLE;
    // 供本管线分配描述符集合的池。
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    // 当前绑定的 uniform、纹理及采样器集合。
    VkDescriptorSet descriptor_ = VK_NULL_HANDLE;
    // 描述符布局与 push constant 范围的组合。
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    // 0 为双面，1/2 分别为普通/镜像变换下的单面。
    std::array<VkPipeline, 3> surface_pipelines_{};
    std::array<VkPipeline, 3> shadow_pipelines_{};
    VkPipeline                wire_pipeline_       = VK_NULL_HANDLE;
    VkPipeline                background_pipeline_ = VK_NULL_HANDLE;
    std::uint64_t             scene_revision_      = std::numeric_limits<std::uint64_t>::max();
};
