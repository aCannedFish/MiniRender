// 线性图像到屏幕显示的转换、FXAA 和预览轮廓。

#pragma once

#include "hdr_targets.hpp"
#include "screen_pass.hpp"
#include <array>
#include <functional>

// 独立显示 pass：从线性 HDR、法线、深度生成 swapchain 图像。
// 浮点纹理用最近邻采样器；FXAA 的双线性插值在感知颜色空间显式实现。
class PostProcess {
  public:
    PostProcess() = default;
    ~PostProcess();
    PostProcess(const PostProcess&)            = delete;
    PostProcess& operator=(const PostProcess&) = delete;
    // 创建并绑定本对象所管理的 GPU 资源，失败时通过异常报告 Vulkan 操作。
    void create(VulkanViewport& viewport, const HdrTargets& targets, VkBuffer uniform_buffer,
                VkDeviceSize uniform_size, VkImageView hdr, VkImageView raw_ao,
                VkImageView filtered_ao);
    // overlay 在显示变换之后、交换链 renderpass 结束之前执行。
    // 录制该 pass 的绘制命令，输出图像交由后续 pass 或显示阶段使用。
    void draw(VulkanViewport& viewport, const std::function<void()>& overlay = {});
    // 首次启用时分配两张显示纹理。缓存内容已应用曝光、显示模式与抗锯齿。
    void prepare_comparison(VulkanViewport& viewport);
    void store_side(VulkanViewport& viewport, std::size_t side, int mode);
    void present_comparison(VulkanViewport& viewport, const std::function<void()>& overlay);
    // 释放独占资源并清空句柄；调用前必须结束 GPU 对这些资源的访问。
    void release();

  private:
    // 正常显示和比较共用最终图形管线，push 值决定输出整图、左右区域或单侧导出。
    void present(VulkanViewport& viewport, VkDescriptorSet set, int split,
                 const std::function<void()>& overlay);
    void update_outline(VulkanViewport& viewport, int mode);
    std::array<ScreenPass, 2>  sides_;
    std::array<ScreenInput, 7> inputs_{};
    VkDescriptorBufferInfo     uniform_info_{};
    VkDescriptorSet            comparison_set_ = VK_NULL_HANDLE;
    // 显示轮廓使用独立附件，HDR 导出继续读取原始合成图。
    // 原色预览的显示轮廓附件与绘制 pass。
    ScreenPass outline_;
    // 是否已为轮廓附件建立可采样布局。
    bool outline_initialized_ = false;
    // 借用的逻辑设备句柄，所有资源必须在设备销毁前释放。
    VkDevice device_ = VK_NULL_HANDLE;
    // 输入图像所使用的 Vulkan 采样器。
    VkSampler sampler_ = VK_NULL_HANDLE;
    // 着色器资源的 binding 与类型声明。
    VkDescriptorSetLayout descriptor_layout_ = VK_NULL_HANDLE;
    // 供本管线分配描述符集合的池。
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    // 当前绑定的 uniform、纹理及采样器集合。
    VkDescriptorSet  descriptor_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_     = VK_NULL_HANDLE;
    // 当前绘制使用的 Vulkan 管线。
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};
