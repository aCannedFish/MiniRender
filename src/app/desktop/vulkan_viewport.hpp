// Vulkan 窗口生命周期、输入交互、渲染调度与图像读回。

#pragma once

#include "gpu_image.hpp"
#include <minirender/core/image.hpp>
#include <minirender/render/ibl.hpp>
#include <minirender/render/point_preview.hpp>
#include <minirender/render/prt.hpp>
#include <minirender/render/raster_mesh.hpp>
#include <minirender/render/sdf_render.hpp>
#include <minirender/render/ssao.hpp>
#include <minirender/scene/gizmo.hpp>
#include <minirender/scene/scene.hpp>
#include <vulkan/vulkan.h>

#include <QElapsedTimer>
#include <QImage>
#include <QWindow>

#include <functional>
#include <memory>
#include <vector>

enum class OfflineImageSource { render, lightfield };

class ViewportRenderer;
class PointPreviewRenderer;
class SdfRenderer;

// 渲染更新的诊断计数，用于检测普通参数编辑是否触发多余的资产上传。
struct SceneUploadStatistics {
    std::uint64_t mesh_uploads         = 0;
    std::uint64_t mesh_allocations     = 0;
    std::uint64_t index_uploads        = 0;
    std::uint64_t vertex_buffer_reuses = 0;
    std::uint64_t descriptor_rebuilds  = 0;
    std::uint64_t scene_updates        = 0;
    double        last_update_ms       = 0;
};

// Qt 提供窗口与平台 surface，本类独占 Vulkan device、交换链和帧同步。
// 首版仅允许一帧在 GPU 执行；呈现完成信号量按交换链图像分配，不能按 CPU 帧号复用。
// 真正的提交延后到 Qt timer，避免在 macOS 的 CALayer expose/事务回调内等待 drawable。
enum class ShadowFilter { hard, pcf, pcss };

class VulkanViewport final : public QWindow {
  public:
    VulkanViewport();
    ~VulkanViewport() override;
    // 回归在独立 Vulkan 图像中执行同一套渲染管线，不依赖可见窗口或呈现队列。
    // 在 Vulkan 初始化前选择离屏或窗口渲染目标；设备建立后拒绝切换。
    void set_offscreen_rendering(bool enabled);
    // 查询是否使用独立离屏图像进行渲染。
    bool offscreen_rendering() const noexcept;
    // 查询设备与当前绘制目标是否满足提交条件。
    bool can_render() const noexcept;

    // 在 GUI 线程更新三角网格场景快照，并请求视口刷新。
    void set_scene(std::shared_ptr<const minirender::SceneSnapshot> scene);
    // 返回三角网格场景快照；引用和句柄由视口保有。
    const std::shared_ptr<const minirender::SceneSnapshot>& scene() const noexcept;
    // 返回已发布场景的版本号，渲染器据此复用或更新资源。
    std::uint64_t scene_revision() const noexcept;
    // 接收后台准备好的网格顶点数据，与原始不可变网格建立缓存关联。
    void supply_raster_mesh(const std::shared_ptr<const minirender::MeshData>& mesh,
                            std::shared_ptr<const minirender::RasterMeshData>  prepared);
    std::shared_ptr<const minirender::RasterMeshData>
    // 取得指定网格的渲染展开数据，缺失时按当前缓存策略准备。
    raster_mesh(const std::shared_ptr<const minirender::MeshData>& mesh);
    // 返回当前视图相机；引用和句柄由视口保有。
    const minirender::Camera& camera() const noexcept;
    // 在 GUI 线程更新当前视图相机，并请求视口刷新。
    void set_camera(const minirender::Camera& camera);
    // 在 GUI 线程更新已准备的点云预览数据，并请求视口刷新。
    void set_point_preview_data(std::shared_ptr<const minirender::PointPreviewData> data);
    // 返回已准备的点云预览数据；引用和句柄由视口保有。
    const std::shared_ptr<const minirender::PointPreviewData>& point_preview_data() const noexcept;
    // 在 GUI 线程更新点云的显示参数，并请求视口刷新。
    void set_point_preview_settings(minirender::PointPreviewSettings settings);
    // 返回点云的显示参数；引用和句柄由视口保有。
    const minirender::PointPreviewSettings& point_preview_settings() const noexcept;
    // 在 GUI 线程更新点云预览是否可见，并请求视口刷新。
    void set_point_preview_visible(bool visible);
    // 查询点云预览是否可见。
    bool point_preview_visible() const noexcept;
    // 将点云相机恢复到能容纳当前预览数据的位置。
    void reset_point_camera();
    void report_point_upload();
    std::uint64_t point_uploads() const noexcept;
    std::function<void(bool)> on_point_preview_changed;
    // 在 GUI 线程更新独立距离场文档，并请求视口刷新。
    void set_sdf_document(std::shared_ptr<const minirender::SdfDocument> document,
                          bool                                           reset_camera = false);
    // 返回独立距离场文档；引用和句柄由视口保有。
    const std::shared_ptr<const minirender::SdfDocument>& sdf_document() const noexcept;
    // 在 GUI 线程更新距离场视图是否可见，并请求视口刷新。
    void set_sdf_visible(bool visible);
    // 查询距离场视图是否可见。
    bool sdf_visible() const noexcept;
    // 在 GUI 线程更新距离场诊断模式，并请求视口刷新。
    void set_sdf_view(minirender::SdfView view);
    // 查询距离场诊断模式。
    minirender::SdfView sdf_view() const noexcept;
    // 在 GUI 线程更新距离场阴影开关，并请求视口刷新。
    void set_sdf_shadows(bool enabled);
    // 查询距离场阴影开关。
    bool sdf_shadows() const noexcept;
    // 返回距离场使用或暂存的独立相机。
    const minirender::Camera& sdf_camera() const noexcept;
    std::function<void(bool)> on_sdf_visible_changed;
    std::function<void()>     on_sdf_display_changed;
    void report_sdf_upload();
    std::uint64_t sdf_uploads() const noexcept;
    // 在 GUI 线程更新方向光阴影开关，并请求视口刷新。
    void set_shadows_enabled(bool enabled);
    // 固定核用纹素半径，PCSS 用方向光的角半径（度）。
    ShadowFilter shadow_filter() const noexcept;
    void set_shadow_filter(ShadowFilter filter);
    // 右侧为 -1 时跟随相机面板的过滤器。
    // -1 使用材质自身的设置，0/1 在对照侧关闭或开启粗糙导体补偿。
    void set_reference_energy(int value);

    int reference_energy() const noexcept {
        return reference_energy_;
    }

    void set_reference_shadow(int filter);

    ShadowFilter effective_shadow(bool reference) const noexcept {
        return reference && reference_shadow_ >= 0 ? static_cast<ShadowFilter>(reference_shadow_)
                                                   : shadow_filter_;
    }

    float light_half_angle() const noexcept;
    void set_light_half_angle(float degrees);
    float shadow_filter_radius() const noexcept;
    void set_shadow_filter_radius(float texels);
    void set_shadow_debug_light(int light);

    int shadow_debug_light() const noexcept {
        return shadow_debug_light_;
    }

    // 面光源阴影在拖动期间使用 2×2 样本，静止时使用 3×3。
    bool area_shadows() const noexcept {
        return area_shadows_;
    }

    void set_area_shadows(bool enabled);

    // 最近一次面光源深度图重建次数与采样布局，供状态检查使用。
    void report_area_shadow(int grid, std::uint64_t updates) noexcept {
        area_grid_    = grid;
        area_updates_ = updates;
    }

    int area_shadow_grid() const noexcept {
        return area_grid_;
    }

    std::uint64_t area_shadow_updates() const noexcept {
        return area_updates_;
    }

    bool navigating() const noexcept {
        return pressed_buttons_ != Qt::NoButton || gizmo_drag_.has_value();
    }

    // 查询方向光阴影开关。
    bool shadows_enabled() const noexcept;
    // 在 GUI 线程更新最终显示的 FXAA 开关，并请求视口刷新。
    void set_fxaa_enabled(bool enabled);
    // 查询最终显示的 FXAA 开关。
    bool fxaa_enabled() const noexcept;
    // 在 GUI 线程更新光照显示曝光 EV，并请求视口刷新。
    void set_exposure(float value);
    // 查询光照显示曝光 EV。
    float exposure() const noexcept;
    static constexpr int base_color_preview_mode = 6;

    static bool uses_lighting(int mode) noexcept {
        return mode == 0 || (mode >= 13 && mode <= 16) || mode == 19 || mode == 20;
    }

    // x、y 使用视空间米，z、w 为每条射线的步数上限与每像素的方向数。
    glm::vec4 screen_trace() const noexcept {
        return screen_trace_;
    }

    bool screen_hiz() const noexcept {
        return screen_hiz_;
    }

    glm::vec2 temporal_settings() const noexcept {
        return temporal_;
    }

    std::uint64_t history_epoch() const noexcept {
        return history_epoch_;
    }

    void set_temporal_settings(glm::vec2 value);
    void reset_history();

    void report_history(unsigned age) noexcept {
        history_age_ = age;
    }

    unsigned history_age() const noexcept {
        return history_age_;
    }

    void set_screen_trace(glm::vec4 settings, bool hiz);
    static constexpr int prt_mode = 12;
    void set_prt(std::shared_ptr<const minirender::PrtBake> bake, minirender::PrtMode mode);
    void set_prt_lighting(const minirender::ShLight& lighting);

    const std::shared_ptr<const minirender::PrtBake>& prt() const noexcept {
        return prt_;
    }

    const minirender::ShLight& prt_lighting() const noexcept {
        return prt_lighting_;
    }

    static constexpr int blinn_phong_mode = 7;

    // 右侧模式为 -1 时使用普通视口；分界位置是窗口宽度的比例。
    int comparison_mode() const noexcept {
        return comparison_mode_;
    }

    void set_comparison_mode(int mode);

    float split_position() const noexcept {
        return split_;
    }

    void set_split_position(float value);

    // 教学灯光的变化只使使用 Blinn–Phong 的一侧过期。
    std::uint64_t comparison_revision(int mode, bool reference) const noexcept {
        return preview_revision_ + (reference ? reference_revision_ : 0) +
               (mode == blinn_phong_mode ? teaching_revision_ : 0);
    }

    std::uint64_t comparison_draws() const noexcept {
        return comparison_draws_;
    }

    void report_comparison_draw() noexcept {
        ++comparison_draws_;
    }

    // 点光源位置使用世界坐标，w 存放强度。表面参数为指数、镜面系数和纹理开关。
    glm::vec4 teaching_light() const noexcept {
        return teaching_light_;
    }

    glm::vec3 teaching_surface() const noexcept {
        return teaching_surface_;
    }

    void set_teaching(glm::vec4 light, glm::vec3 surface);
    // 导出使用同一相机的完整显示图或指定侧的原始线性 HDR。
    QImage grab_comparison_side(int side);
    minirender::Image grab_comparison_hdr(int side);

    int export_side() const noexcept {
        return export_side_;
    }

    // 在 GUI 线程更新原色轮廓开关，并请求视口刷新。
    void set_preview_outline(bool enabled);
    // 查询原色轮廓开关。
    bool preview_outline() const noexcept;
    // 在 GUI 线程更新原色轮廓强度（0–1），并请求视口刷新。
    void set_preview_outline_strength(float strength);
    // 查询原色轮廓强度（0–1）。
    float preview_outline_strength() const noexcept;
    // 在 GUI 线程更新原色接触遮蔽开关，并请求视口刷新。
    void set_preview_occlusion(bool enabled);
    // 查询原色接触遮蔽开关。
    bool preview_occlusion() const noexcept;
    // 在 GUI 线程更新实时视口的显示模式编号，并请求视口刷新。
    void set_view_mode(int mode);
    // 查询实时视口的显示模式编号。
    int view_mode() const noexcept;
    // 在 GUI 线程更新SSAO 采样与合成参数，并请求视口刷新。
    void set_ssao_settings(minirender::SsaoSettings settings);
    // 返回SSAO 采样与合成参数；引用和句柄由视口保有。
    const minirender::SsaoSettings& ssao_settings() const noexcept;
    // 接收共享只读环境预处理图和 BRDF LUT，触发 GPU 资源更新。
    void set_ibl_data(std::shared_ptr<const minirender::IblEnvironment> maps,
                      std::shared_ptr<const minirender::Image>          lut);
    // 返回当前环境预处理结果的共享只读引用。
    const std::shared_ptr<const minirender::IblEnvironment>& ibl_maps() const noexcept;
    // 返回当前 BRDF 积分表的共享只读引用。
    const std::shared_ptr<const minirender::Image>& ibl_lut() const noexcept;
    // 在 GUI 线程更新环境图背景开关，并请求视口刷新。
    void set_environment_background(bool enabled);
    // 查询环境图背景开关。
    bool environment_background() const noexcept;
    void report_ibl_update();
    std::uint64_t ibl_texture_updates() const noexcept;
    // 在 GUI 线程更新CPU 结果是否占据当前视口，并请求视口刷新。
    void set_offline_visible(bool visible);
    // 查询CPU 结果是否占据当前视口。
    bool offline_visible() const noexcept;
    // 在 GUI 线程更新当前 CPU 线性预览图像，并请求视口刷新。
    void set_offline_image(std::shared_ptr<const minirender::Image> image,
                           OfflineImageSource source = OfflineImageSource::render);
    // 查询当前 CPU 图像来自离线渲染还是光场工具。
    OfflineImageSource offline_source() const noexcept;

    // 返回显示来源版本，供后台完成回调判断是否仍可切换前台。
    std::uint64_t display_revision() const noexcept {
        return display_revision_;
    }

    // 返回当前 CPU 线性预览图像；引用和句柄由视口保有。
    const std::shared_ptr<const minirender::Image>& offline_image() const noexcept;
    std::function<void(bool)>                      on_display_changed;
    std::function<void(const minirender::Camera&)> on_camera_changed;
    std::function<void(const std::string&)>        on_object_picked;
    // 操纵器预览与历史由 Workbench/SceneEditor 管理；视口拥有几何命中和鼠标手势。
    // 在 GUI 线程更新选择、移动或旋转操纵模式，并请求视口刷新。
    void set_gizmo_mode(minirender::GizmoMode mode);
    // 查询选择、移动或旋转操纵模式。
    minirender::GizmoMode gizmo_mode() const noexcept;
    // 更新操纵器目标标识与变换，空变换表示没有可操纵对象。
    void set_gizmo_target(std::string object_id, std::optional<minirender::Transform> transform);
    // 返回按当前相机生成的操纵器布局。
    const minirender::GizmoLayout& gizmo_layout() const noexcept;
    // 返回鼠标当前命中的操纵手柄。
    minirender::GizmoHandle highlighted_gizmo_handle() const noexcept;
    // 查询是否正在执行物体变换手势。
    bool gizmo_dragging() const noexcept;
    // 结束当前手势；commit 决定提交变换或恢复起点。
    void finish_gizmo_drag(bool commit);
    std::function<bool()>                             on_transform_begin;
    std::function<bool(const minirender::Transform&)> on_transform_preview;
    std::function<void(bool)>                         on_transform_end;
    std::function<void(minirender::GizmoMode)>        on_gizmo_mode_changed;
    std::function<void(const std::string&)>           on_interaction_error;

    bool isValid() const noexcept;
    VkDevice device() const noexcept;
    VkPhysicalDevice physicalDevice() const noexcept;
    const VkPhysicalDeviceProperties* physicalDeviceProperties() const noexcept;
    VkRenderPass defaultRenderPass() const noexcept;
    VkCommandBuffer currentCommandBuffer() const noexcept;
    VkFramebuffer currentFramebuffer() const;
    QSize swapChainImageSize() const noexcept;
    bool supportsGrab() const noexcept;
    // 完成必要绘制后读取当前显示图像，包含显示变换和叠加效果。
    QImage grab();
    // 实时读回 HDR 附件；离线返回当前 CPU 预览，全尺寸导出由 OfflinePanel 处理。
    // 读取当前显示来源的线性浮点图像。
    minirender::Image grab_hdr();

    void frameReady();
    void requestUpdate();
    void report_frame();
    void report_swapchain();
    // 返回已完成的渲染帧计数。
    int rendered_frames() const noexcept;
    // 返回尺寸相关资源的创建代数。
    int resource_generations() const noexcept;
    // 返回资源上传和缓存复用计数，供诊断与回归使用。
    const SceneUploadStatistics& upload_statistics() const noexcept;
    void report_mesh_upload();
    void report_mesh_allocation();
    void report_vertex_buffer_reuse();
    void report_descriptor_rebuild();
    void report_scene_update(double milliseconds);
    // 返回最新预览版本尚未完成显示的等待时间。
    qint64 preview_wait_ms() const noexcept;
    // 返回当前未能提交或显示下一帧的状态说明。
    QString frame_wait_reason() const;
    void report_failure(const QString& error);
    // 返回渲染器记录的失败信息。
    QString failure() const;

  protected:
    bool event(QEvent* event) override;
    void exposeEvent(QExposeEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

  private:
    // 建立 Vulkan 设备、队列、同步资源与渲染器。
    void initialize();
    // 在尺寸或呈现目标变化后重建交换链及尺寸相关资源。
    void recreate_swapchain();
    // 结束相关 GPU 使用并释放交换链与附件。
    void release_swapchain();
    // 收回渲染器、同步对象和独占设备资源。
    void release();
    // 选择当前显示来源，录制和提交一帧，必要时执行阻塞读回。
    void render_frame(bool blocking_capture = false);
    // 录制当前显示附件到主机缓冲的复制。
    void record_readback();
    // 在复制完成后生成 CPU 可读取的显示图像。
    void finish_readback();
    // 增加预览版本，并开始跟踪新内容的显示等待。
    void mark_preview_changed();
    // 根据当前相机、对象和模式重新计算操纵器布局。
    void rebuild_gizmo();
    // 查找满足内存类型掩码和属性要求的设备内存索引。
    std::uint32_t memory_type(std::uint32_t bits, VkMemoryPropertyFlags flags) const;

    std::unique_ptr<ViewportRenderer>                   renderer_;
    std::unique_ptr<ViewportRenderer>                   offline_renderer_;
    std::unique_ptr<PointPreviewRenderer>               point_renderer_;
    std::unique_ptr<SdfRenderer>                        sdf_renderer_;
    std::shared_ptr<const minirender::SdfDocument>      sdf_document_;
    minirender::Camera                                  sdf_camera_;
    minirender::SdfView                                 sdf_view_    = minirender::SdfView::surface;
    bool                                                sdf_shadows_ = true;
    bool                                                sdf_visible_ = false;
    std::uint64_t                                       sdf_uploads_ = 0;
    std::shared_ptr<const minirender::PointPreviewData> point_data_;
    minirender::PointPreviewSettings                    point_settings_;
    minirender::Camera                                  scene_camera_before_tools_;
    minirender::Camera                                  point_camera_;
    bool                                                point_visible_ = false;
    std::uint64_t                                       point_uploads_ = 0;
    std::shared_ptr<const minirender::PrtBake>          prt_;
    minirender::PrtMode                                 prt_kind_ = minirender::PrtMode::shadowed;
    minirender::ShLight                                 prt_lighting_{};
    std::shared_ptr<const minirender::Image>            offline_image_;
    bool                                                offline_visible_  = false;
    std::uint64_t                                       display_revision_ = 0;
    OfflineImageSource                               offline_source_ = OfflineImageSource::render;
    std::shared_ptr<const minirender::SceneSnapshot> scene_;
    minirender::RasterMeshCache                      raster_meshes_;
    minirender::Camera                               camera_;
    // 当前已发布场景版本，供渲染器检测参数更新。
    std::uint64_t         scene_revision_ = 0;
    SceneUploadStatistics upload_statistics_;
    // 最新请求显示的内容版本。
    std::uint64_t preview_revision_ = 0;
    // 最近一次 GPU 提交使用的预览版本。
    std::uint64_t submitted_preview_revision_ = 0;
    // 已完成 GPU 提交的预览版本。
    std::uint64_t completed_preview_revision_ = 0;
    // 记录等待新预览完成的时间。
    QElapsedTimer            preview_wait_;
    QString                  frame_wait_reason_;
    bool                     area_shadows_ = true;
    int                      area_grid_    = 0;
    std::uint64_t            area_updates_ = 0;
    glm::vec2                temporal_{.9F, 1.F};
    std::uint64_t            history_epoch_ = 0;
    unsigned                 history_age_   = 0;
    glm::vec4                screen_trace_{5.F, .03F, 160.F, 8.F};
    bool                     screen_hiz_       = true;
    ShadowFilter             shadow_filter_    = ShadowFilter::pcf;
    float                    light_half_angle_ = 3.F, shadow_filter_radius_ = 1.F;
    int                      reference_energy_   = -1;
    int                      shadow_debug_light_ = 0, reference_shadow_ = -1;
    std::uint64_t            reference_revision_ = 0;
    bool                     shadows_enabled_    = true;
    bool                     fxaa_enabled_       = true;
    std::uint64_t            teaching_revision_ = 0, comparison_draws_ = 0;
    int                      comparison_mode_ = -1;
    int                      export_side_     = -1;
    float                    split_           = 0.5F;
    bool                     dragging_split_  = false;
    glm::vec4                teaching_light_{1.5F, 2.5F, 2.0F, 10.F};
    glm::vec3                teaching_surface_{35.F, 0.25F, 1.F};
    float                    exposure_                 = 0.0F;
    int                      view_mode_                = 0;
    bool                     preview_occlusion_        = true;
    bool                     preview_outline_          = true;
    float                    preview_outline_strength_ = 0.35F;
    minirender::SsaoSettings ssao_;
    std::shared_ptr<const minirender::IblEnvironment> ibl_maps_;
    std::shared_ptr<const minirender::Image>          ibl_lut_;
    bool                                              environment_background_ = true;
    std::uint64_t                                     ibl_texture_updates_    = 0;
    QPointF                                           mouse_origin_;
    QPointF                                           last_mouse_;
    bool                                              dragged_ = false;
    // 仅记录本窗口接收 press 的按钮，避免外部拖动启动导航。
    Qt::MouseButtons                     pressed_buttons_ = Qt::NoButton;
    minirender::GizmoMode                gizmo_mode_      = minirender::GizmoMode::select;
    std::string                          gizmo_target_id_;
    std::optional<minirender::Transform> gizmo_target_;
    minirender::GizmoLayout              gizmo_layout_;
    minirender::GizmoHandle              gizmo_hover_ = minirender::GizmoHandle::none;
    std::optional<minirender::GizmoDrag> gizmo_drag_;
    VkPhysicalDevice                     physical_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties           properties_{};
    VkDevice                             device_       = VK_NULL_HANDLE;
    VkQueue                              queue_        = VK_NULL_HANDLE;
    std::uint32_t                        queue_family_ = 0;
    VkSurfaceKHR    surface_   = VK_NULL_HANDLE; // Qt instance/window 拥有，不在此销毁。
    VkSwapchainKHR  swapchain_ = VK_NULL_HANDLE;
    bool            offscreen_ = false;
    GpuImage        offscreen_color_;
    VkFormat        color_format_ = VK_FORMAT_UNDEFINED;
    VkFormat        depth_format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D      extent_{};
    VkRenderPass    render_pass_  = VK_NULL_HANDLE;
    VkCommandPool   command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_      = VK_NULL_HANDLE;
    // 单帧在途同步，CPU 修改资源前等待该 fence。
    VkFence frame_fence_ = VK_NULL_HANDLE;
    // 交换链图像获取完成信号量。
    VkSemaphore acquired_ = VK_NULL_HANDLE;
    // 按交换链图像分配的呈现等待信号量。
    std::vector<VkSemaphore>   rendered_;
    std::vector<VkImage>       images_;
    std::vector<VkImageView>   views_;
    std::vector<VkFramebuffer> framebuffers_;
    VkImage                    depth_           = VK_NULL_HANDLE;
    VkImageView                depth_view_      = VK_NULL_HANDLE;
    VkDeviceMemory             depth_memory_    = VK_NULL_HANDLE;
    VkBuffer                   readback_        = VK_NULL_HANDLE;
    VkDeviceMemory             readback_memory_ = VK_NULL_HANDLE;
    QImage                     captured_;
    std::uint32_t              image_index_ = 0;
    // 当前视口内容是否需要重新绘制。
    bool dirty_ = true;
    // 是否已有等待 Qt 事件循环执行的渲染请求。
    bool scheduled_ = false;
    // 当前帧是否需要读回屏幕像素。
    bool capture_  = false;
    bool can_grab_ = false;
    // 已完成的渲染帧计数。
    int frame_count_ = 0;
    // 尺寸相关资源重建次数。
    int swapchain_count_ = 0;
    // 最后记录的渲染错误，供 UI 提示及测试判定。
    QString failure_;
};
