#pragma once

#include "gpu_image.hpp"
#include <minirender/core/image.hpp>
#include <minirender/render/ibl.hpp>
#include <minirender/render/raster_mesh.hpp>
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

class ViewportRenderer;

// 渲染更新的诊断计数，用于检测普通参数编辑是否触发多余的资产上传。
struct SceneUploadStatistics {
    std::uint64_t mesh_uploads = 0;
    std::uint64_t descriptor_rebuilds = 0;
    std::uint64_t scene_updates = 0;
    double last_update_ms = 0;
};

// Qt 提供窗口与平台 surface，本类独占 Vulkan device、交换链和帧同步。
// 首版仅允许一帧在 GPU 执行；呈现完成信号量按交换链图像分配，不能按 CPU 帧号复用。
// 真正的提交延后到 Qt timer，避免在 macOS 的 CALayer expose/事务回调内等待 drawable。
class VulkanViewport final : public QWindow {
  public:
    VulkanViewport();
    ~VulkanViewport() override;
    // 回归在独立 Vulkan 图像中执行同一套渲染管线，不依赖可见窗口或呈现队列。
    void set_offscreen_rendering(bool enabled);
    bool offscreen_rendering() const noexcept;
    bool can_render() const noexcept;

    void set_scene(std::shared_ptr<const minirender::SceneSnapshot> scene);
    const std::shared_ptr<const minirender::SceneSnapshot>& scene() const noexcept;
    std::uint64_t scene_revision() const noexcept;
    void supply_raster_mesh(const std::shared_ptr<const minirender::MeshData>& mesh,
                            std::shared_ptr<const minirender::RasterMeshData> prepared);
    std::shared_ptr<const minirender::RasterMeshData>
    raster_mesh(const std::shared_ptr<const minirender::MeshData>& mesh);
    const minirender::Camera& camera() const noexcept;
    void set_camera(const minirender::Camera& camera);
    void set_shadows_enabled(bool enabled);
    bool shadows_enabled() const noexcept;
    void set_fxaa_enabled(bool enabled);
    bool fxaa_enabled() const noexcept;
    void set_exposure(float value);
    float exposure() const noexcept;
    void set_view_mode(int mode);
    int view_mode() const noexcept;
    void set_ssao_settings(minirender::SsaoSettings settings);
    const minirender::SsaoSettings& ssao_settings() const noexcept;
    void set_ibl_data(std::shared_ptr<const minirender::IblEnvironment> maps,
                      std::shared_ptr<const minirender::Image> lut);
    const std::shared_ptr<const minirender::IblEnvironment>& ibl_maps() const noexcept;
    const std::shared_ptr<const minirender::Image>& ibl_lut() const noexcept;
    void set_environment_background(bool enabled);
    bool environment_background() const noexcept;
    void report_ibl_update();
    std::uint64_t ibl_texture_updates() const noexcept;
    void set_offline_visible(bool visible);
    bool offline_visible() const noexcept;
    void set_offline_image(std::shared_ptr<const minirender::Image> image);
    const std::shared_ptr<const minirender::Image>& offline_image() const noexcept;
    std::function<void(bool)> on_display_changed;
    std::function<void(const minirender::Camera&)> on_camera_changed;
    std::function<void(const std::string&)> on_object_picked;
    // 操纵器预览与历史由 Workbench/SceneEditor 管理；视口拥有几何命中和鼠标手势。
    void set_gizmo_mode(minirender::GizmoMode mode);
    minirender::GizmoMode gizmo_mode() const noexcept;
    void set_gizmo_target(std::string object_id, std::optional<minirender::Transform> transform);
    const minirender::GizmoLayout& gizmo_layout() const noexcept;
    minirender::GizmoHandle highlighted_gizmo_handle() const noexcept;
    bool gizmo_dragging() const noexcept;
    void finish_gizmo_drag(bool commit);
    std::function<bool()> on_transform_begin;
    std::function<bool(const minirender::Transform&)> on_transform_preview;
    std::function<void(bool)> on_transform_end;
    std::function<void(minirender::GizmoMode)> on_gizmo_mode_changed;
    std::function<void(const std::string&)> on_interaction_error;

    bool isValid() const noexcept;
    VkDevice device() const noexcept;
    VkPhysicalDevice physicalDevice() const noexcept;
    const VkPhysicalDeviceProperties* physicalDeviceProperties() const noexcept;
    VkRenderPass defaultRenderPass() const noexcept;
    VkCommandBuffer currentCommandBuffer() const noexcept;
    VkFramebuffer currentFramebuffer() const;
    QSize swapChainImageSize() const noexcept;
    bool supportsGrab() const noexcept;
    QImage grab();
    // 实时读回 HDR 附件；离线返回当前 CPU 预览，全尺寸导出由 OfflinePanel 处理。
    minirender::Image grab_hdr();

    void frameReady();
    void requestUpdate();
    void report_frame();
    void report_swapchain();
    int rendered_frames() const noexcept;
    int resource_generations() const noexcept;
    const SceneUploadStatistics& upload_statistics() const noexcept;
    void report_mesh_upload();
    void report_descriptor_rebuild();
    void report_scene_update(double milliseconds);
    qint64 preview_wait_ms() const noexcept;
    QString frame_wait_reason() const;
    void report_failure(const QString& error);
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
    void initialize();
    void recreate_swapchain();
    void release_swapchain();
    void release();
    void render_frame(bool blocking_capture = false);
    void record_readback();
    void finish_readback();
    void mark_preview_changed();
    void rebuild_gizmo();
    std::uint32_t memory_type(std::uint32_t bits, VkMemoryPropertyFlags flags) const;

    std::unique_ptr<ViewportRenderer> renderer_;
    std::unique_ptr<ViewportRenderer> offline_renderer_;
    std::shared_ptr<const minirender::Image> offline_image_;
    bool offline_visible_ = false;
    std::shared_ptr<const minirender::SceneSnapshot> scene_;
    minirender::RasterMeshCache raster_meshes_;
    minirender::Camera camera_;
    std::uint64_t scene_revision_ = 0;
    SceneUploadStatistics upload_statistics_;
    std::uint64_t preview_revision_ = 0;
    std::uint64_t submitted_preview_revision_ = 0;
    std::uint64_t completed_preview_revision_ = 0;
    QElapsedTimer preview_wait_;
    QString frame_wait_reason_;
    bool shadows_enabled_ = true;
    bool fxaa_enabled_ = true;
    float exposure_ = 0.0F;
    int view_mode_ = 0;
    minirender::SsaoSettings ssao_;
    std::shared_ptr<const minirender::IblEnvironment> ibl_maps_;
    std::shared_ptr<const minirender::Image> ibl_lut_;
    bool environment_background_ = true;
    std::uint64_t ibl_texture_updates_ = 0;
    QPointF mouse_origin_;
    QPointF last_mouse_;
    bool dragged_ = false;
    Qt::MouseButtons pressed_buttons_ = Qt::NoButton;
    minirender::GizmoMode gizmo_mode_ = minirender::GizmoMode::select;
    std::string gizmo_target_id_;
    std::optional<minirender::Transform> gizmo_target_;
    minirender::GizmoLayout gizmo_layout_;
    minirender::GizmoHandle gizmo_hover_ = minirender::GizmoHandle::none;
    std::optional<minirender::GizmoDrag> gizmo_drag_;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties_{};
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE; // Qt instance/window 拥有，不在此销毁。
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    bool offscreen_ = false;
    GpuImage offscreen_color_;
    VkFormat color_format_ = VK_FORMAT_UNDEFINED;
    VkFormat depth_format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_ = VK_NULL_HANDLE;
    VkFence frame_fence_ = VK_NULL_HANDLE;
    VkSemaphore acquired_ = VK_NULL_HANDLE;
    std::vector<VkSemaphore> rendered_;
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<VkFramebuffer> framebuffers_;
    VkImage depth_ = VK_NULL_HANDLE;
    VkImageView depth_view_ = VK_NULL_HANDLE;
    VkDeviceMemory depth_memory_ = VK_NULL_HANDLE;
    VkBuffer readback_ = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory_ = VK_NULL_HANDLE;
    QImage captured_;
    std::uint32_t image_index_ = 0;
    bool dirty_ = true;
    bool scheduled_ = false;
    bool capture_ = false;
    bool can_grab_ = false;
    int frame_count_ = 0;
    int swapchain_count_ = 0;
    QString failure_;
};
