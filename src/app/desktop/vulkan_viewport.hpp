#pragma once

#include <minirender/core/image.hpp>
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

    void set_scene(std::shared_ptr<const minirender::SceneSnapshot> scene);
    const std::shared_ptr<const minirender::SceneSnapshot>& scene() const noexcept;
    std::uint64_t scene_revision() const noexcept;
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
    std::function<void(const minirender::Camera&)> on_camera_changed;
    std::function<void(const std::string&)> on_object_picked;

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
    std::uint32_t memory_type(std::uint32_t bits, VkMemoryPropertyFlags flags) const;

    std::unique_ptr<ViewportRenderer> renderer_;
    std::shared_ptr<const minirender::SceneSnapshot> scene_;
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
    QPointF mouse_origin_;
    QPointF last_mouse_;
    bool dragged_ = false;
    Qt::MouseButtons pressed_buttons_ = Qt::NoButton;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties_{};
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE; // Qt instance/window 拥有，不在此销毁。
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
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
