#pragma once

#include <vulkan/vulkan.h>

#include <QImage>
#include <QWindow>

#include <memory>
#include <vector>

class DiagnosticRenderer;

// Qt 提供窗口与平台 surface，本类独占 Vulkan device、交换链和帧同步。
// 首版仅允许一帧在 GPU 执行；呈现完成信号量按交换链图像分配，不能按 CPU 帧号复用。
// 真正的提交延后到 Qt timer，避免在 macOS 的 CALayer expose/事务回调内等待 drawable。
class VulkanViewport final : public QWindow {
  public:
    VulkanViewport();
    ~VulkanViewport() override;

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

    void frameReady();
    void requestUpdate();
    void report_frame();
    void report_swapchain();
    int rendered_frames() const noexcept;
    int resource_generations() const noexcept;
    void report_failure(const QString& error);
    QString failure() const;

  protected:
    bool event(QEvent* event) override;
    void exposeEvent(QExposeEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    void initialize();
    void recreate_swapchain();
    void release_swapchain();
    void release();
    void render_frame();
    void record_readback();
    void finish_readback();
    std::uint32_t memory_type(std::uint32_t bits, VkMemoryPropertyFlags flags) const;

    std::unique_ptr<DiagnosticRenderer> renderer_;
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
