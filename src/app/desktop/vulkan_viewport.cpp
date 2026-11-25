// Vulkan 设备与窗口生命周期、帧提交、交换链重建与图像读回。
// 本模块接口见 vulkan_viewport.hpp。

#include "vulkan_viewport.hpp"
#include "diagnostic_renderer.hpp"
#include "offline_renderer.hpp"
#include "point_preview_renderer.hpp"
#include "scene_renderer.hpp"
#include "sdf_renderer.hpp"
#include "vulkan_utils.hpp"

#include <QDebug>
#include <QPlatformSurfaceEvent>
#include <QTimer>
#include <QVulkanInstance>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

VulkanViewport::VulkanViewport() {
    setSurfaceType(QSurface::VulkanSurface);
}

VulkanViewport::~VulkanViewport() {
    release();
}

void VulkanViewport::set_offscreen_rendering(bool enabled) {
    if (device_ != VK_NULL_HANDLE) {
        throw std::logic_error("Select the rendering target before Vulkan initialization");
    }
    offscreen_ = enabled;
}

bool VulkanViewport::offscreen_rendering() const noexcept {
    return offscreen_;
}

bool VulkanViewport::can_render() const noexcept {
    return offscreen_ || isExposed();
}

void VulkanViewport::initialize() {
    if (!offscreen_) {
        surface_ = QVulkanInstance::surfaceForWindow(this);
        if (surface_ == VK_NULL_HANDLE) {
            throw std::runtime_error("Qt could not create a Vulkan surface");
        }
    }
    std::uint32_t count = 0;
    check_vk(vkEnumeratePhysicalDevices(vulkanInstance()->vkInstance(), &count, nullptr),
             "list GPUs");
    std::vector<VkPhysicalDevice> devices(count);
    check_vk(vkEnumeratePhysicalDevices(vulkanInstance()->vkInstance(), &count, devices.data()),
             "get GPUs");
    for (const auto candidate : devices) {
        std::uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
        std::vector<VkQueueFamilyProperties> properties(families);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, properties.data());
        for (std::uint32_t index = 0; index < families; ++index) {
            VkBool32 present = offscreen_ ? VK_TRUE : VK_FALSE;
            if (!offscreen_) {
                check_vk(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, index, surface_, &present),
                         "present support");
            }
            if (present && (properties[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
                physical_     = candidate;
                queue_family_ = index;
                break;
            }
        }
        if (physical_ != VK_NULL_HANDLE) {
            break;
        }
    }
    if (physical_ == VK_NULL_HANDLE) {
        throw std::runtime_error("No graphics/present queue is available");
    }
    vkGetPhysicalDeviceProperties(physical_, &properties_);
    qInfo() << "Vulkan device:" << properties_.deviceName;
    check_vk(vkEnumerateDeviceExtensionProperties(physical_, nullptr, &count, nullptr),
             "list extensions");
    std::vector<VkExtensionProperties> extensions(count);
    check_vk(vkEnumerateDeviceExtensionProperties(physical_, nullptr, &count, extensions.data()),
             "get extensions");
    std::vector<const char*> enabled;
    if (!offscreen_) {
        enabled.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
    for (const auto& extension : extensions) {
        if (std::strcmp(extension.extensionName, "VK_KHR_portability_subset") == 0) {
            enabled.push_back("VK_KHR_portability_subset");
        }
    }
    const float             priority = 1.0F;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = queue_family_;
    queue_info.queueCount       = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{};
    device_info.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount    = 1;
    device_info.pQueueCreateInfos       = &queue_info;
    device_info.enabledExtensionCount   = static_cast<std::uint32_t>(enabled.size());
    device_info.ppEnabledExtensionNames = enabled.data();
    check_vk(vkCreateDevice(physical_, &device_info, nullptr, &device_), "create device");
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    VkCommandPoolCreateInfo pool{};
    pool.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.queueFamilyIndex = queue_family_;
    pool.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    check_vk(vkCreateCommandPool(device_, &pool, nullptr, &command_pool_), "create command pool");
    VkCommandBufferAllocateInfo allocate{};
    allocate.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool        = command_pool_;
    allocate.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    check_vk(vkAllocateCommandBuffers(device_, &allocate, &command_), "allocate command buffer");
    VkFenceCreateInfo fence{};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    check_vk(vkCreateFence(device_, &fence, nullptr, &frame_fence_), "create frame fence");
    VkSemaphoreCreateInfo semaphore{};
    semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    check_vk(vkCreateSemaphore(device_, &semaphore, nullptr, &acquired_),
             "create acquire semaphore");
    if (scene_) {
        renderer_ = std::make_unique<SceneRenderer>(this);
    } else {
        renderer_ = std::make_unique<DiagnosticRenderer>(this);
    }
    renderer_->initResources();
    offline_renderer_ = std::make_unique<OfflineRenderer>(this);
    offline_renderer_->initResources();
    point_renderer_ = std::make_unique<PointPreviewRenderer>(this);
    point_renderer_->initResources();
    sdf_renderer_ = std::make_unique<SdfRenderer>(this);
    sdf_renderer_->initResources();
}

void VulkanViewport::recreate_swapchain() {
    check_vk(vkDeviceWaitIdle(device_), "wait before swapchain recreation");
    release_swapchain();
    if (offscreen_) {
        extent_ = {static_cast<std::uint32_t>(std::max(1, qRound(width() * devicePixelRatio()))),
                   static_cast<std::uint32_t>(std::max(1, qRound(height() * devicePixelRatio())))};
        color_format_ = VK_FORMAT_B8G8R8A8_SRGB;
        offscreen_color_.create(device_, physical_, extent_, color_format_,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT);
        images_   = {offscreen_color_.image()};
        can_grab_ = true;
    } else {
        VkSurfaceCapabilitiesKHR capabilities{};
        check_vk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &capabilities),
                 "surface capabilities");
        std::uint32_t count = 0;
        check_vk(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, nullptr),
                 "surface formats");
        std::vector<VkSurfaceFormatKHR> formats(count);
        check_vk(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, formats.data()),
                 "get formats");
        const auto found = std::find_if(formats.begin(), formats.end(), [](const auto& format) {
            return (format.format == VK_FORMAT_B8G8R8A8_SRGB ||
                    format.format == VK_FORMAT_R8G8B8A8_SRGB) &&
                   format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        });
        if (found == formats.end()) {
            throw std::runtime_error("An sRGB swapchain format is required");
        }
        color_format_ = found->format;
        extent_       = capabilities.currentExtent;
        if (extent_.width == std::numeric_limits<std::uint32_t>::max()) {
            extent_.width =
                std::clamp(static_cast<std::uint32_t>(width() * devicePixelRatio()),
                           capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            extent_.height =
                std::clamp(static_cast<std::uint32_t>(height() * devicePixelRatio()),
                           capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        }
        std::uint32_t desired = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount != 0) {
            desired = std::min(desired, capabilities.maxImageCount);
        }
        VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        for (auto choice :
             {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
              VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR}) {
            if ((capabilities.supportedCompositeAlpha & choice) != 0) {
                alpha = choice;
                break;
            }
        }
        VkSwapchainCreateInfoKHR create{};
        create.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        create.surface          = surface_;
        create.minImageCount    = desired;
        create.imageFormat      = color_format_;
        create.imageColorSpace  = found->colorSpace;
        create.imageExtent      = extent_;
        create.imageArrayLayers = 1;
        create.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        can_grab_ = (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
        if (can_grab_) {
            create.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        }
        create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        create.preTransform     = capabilities.currentTransform;
        create.compositeAlpha   = alpha;
        create.presentMode      = VK_PRESENT_MODE_FIFO_KHR;
        create.clipped          = VK_TRUE;
        check_vk(vkCreateSwapchainKHR(device_, &create, nullptr, &swapchain_), "create swapchain");
        check_vk(vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr),
                 "count swapchain images");
        images_.resize(count);
        check_vk(vkGetSwapchainImagesKHR(device_, swapchain_, &count, images_.data()),
                 "get swapchain images");
    }
    views_.resize(images_.size(), VK_NULL_HANDLE);
    rendered_.resize(images_.size(), VK_NULL_HANDLE);
    for (std::size_t index = 0; index < images_.size(); ++index) {
        if (offscreen_) {
            views_[index] = offscreen_color_.view();
            continue;
        }
        VkImageViewCreateInfo view{};
        view.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image            = images_[index];
        view.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        view.format           = color_format_;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check_vk(vkCreateImageView(device_, &view, nullptr, &views_[index]),
                 "create swapchain view");
        VkSemaphoreCreateInfo semaphore{};
        semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        check_vk(vkCreateSemaphore(device_, &semaphore, nullptr, &rendered_[index]),
                 "create image semaphore");
    }

    depth_format_ = VK_FORMAT_UNDEFINED;
    for (auto format : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM}) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physical_, format, &properties);
        if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) !=
            0) {
            depth_format_ = format;
            break;
        }
    }
    if (depth_format_ == VK_FORMAT_UNDEFINED) {
        throw std::runtime_error("No depth attachment format is supported");
    }
    VkImageCreateInfo depth_info{};
    depth_info.sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    depth_info.imageType   = VK_IMAGE_TYPE_2D;
    depth_info.format      = depth_format_;
    depth_info.extent      = {extent_.width, extent_.height, 1};
    depth_info.mipLevels   = 1;
    depth_info.arrayLayers = 1;
    depth_info.samples     = VK_SAMPLE_COUNT_1_BIT;
    depth_info.tiling      = VK_IMAGE_TILING_OPTIMAL;
    depth_info.usage       = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    check_vk(vkCreateImage(device_, &depth_info, nullptr, &depth_), "create depth image");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, depth_, &requirements);
    VkMemoryAllocateInfo memory{};
    memory.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memory.allocationSize  = requirements.size;
    memory.memoryTypeIndex = find_memory_type(physical_, requirements.memoryTypeBits,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check_vk(vkAllocateMemory(device_, &memory, nullptr, &depth_memory_), "allocate depth memory");
    check_vk(vkBindImageMemory(device_, depth_, depth_memory_, 0), "bind depth memory");
    VkImageViewCreateInfo view{};
    view.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image            = depth_;
    view.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    view.format           = depth_format_;
    view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    check_vk(vkCreateImageView(device_, &view, nullptr, &depth_view_), "create depth view");

    std::array<VkAttachmentDescription, 2> attachments{};
    attachments[0].format         = color_format_;
    attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[1]                = attachments[0];
    attachments[1].format         = depth_format_;
    attachments[1].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color_reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_reference{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription  subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = 1;
    subpass.pColorAttachments       = &color_reference;
    subpass.pDepthStencilAttachment = &depth_reference;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = dependency.srcStageMask;
    dependency.dstAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo pass{};
    pass.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pass.attachmentCount = static_cast<std::uint32_t>(attachments.size());
    pass.pAttachments    = attachments.data();
    pass.subpassCount    = 1;
    pass.pSubpasses      = &subpass;
    pass.dependencyCount = 1;
    pass.pDependencies   = &dependency;
    check_vk(vkCreateRenderPass(device_, &pass, nullptr, &render_pass_), "create render pass");
    framebuffers_.resize(images_.size(), VK_NULL_HANDLE);
    for (std::size_t index = 0; index < images_.size(); ++index) {
        std::array<VkImageView, 2> attachment_views{views_[index], depth_view_};
        VkFramebufferCreateInfo    framebuffer{};
        framebuffer.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer.renderPass      = render_pass_;
        framebuffer.attachmentCount = static_cast<std::uint32_t>(attachment_views.size());
        framebuffer.pAttachments    = attachment_views.data();
        framebuffer.width           = extent_.width;
        framebuffer.height          = extent_.height;
        framebuffer.layers          = 1;
        check_vk(vkCreateFramebuffer(device_, &framebuffer, nullptr, &framebuffers_[index]),
                 "create framebuffer");
    }
    renderer_->initSwapChainResources();
    offline_renderer_->initSwapChainResources();
    point_renderer_->initSwapChainResources();
    sdf_renderer_->initSwapChainResources();
    dirty_ = false;
}

void VulkanViewport::render_frame(bool blocking_capture) {
    if (!can_render() || width() <= 0 || height() <= 0 || !failure_.isEmpty()) {
        return;
    }
    try {
        if (device_ == VK_NULL_HANDLE) {
            initialize();
        }
        // 普通帧只轮询，GPU 忙时将控制权交还给 Qt。显式截图使用有限等待。
        const std::uint64_t timeout = blocking_capture ? 2'000'000'000ULL : 0;
        const auto          ready   = vkWaitForFences(device_, 1, &frame_fence_, VK_TRUE, timeout);
        if (ready == VK_TIMEOUT) {
            frame_wait_reason_ = "GPU fence pending";
            requestUpdate();
            return;
        }
        check_vk(ready, "wait frame fence");
        point_renderer_->discard_if_unused();
        // 连续拖拽会不断发布新版本。只要旧版本持续完成，就没有等待停滞。
        if (completed_preview_revision_ != submitted_preview_revision_) {
            preview_wait_.restart();
        }
        completed_preview_revision_ = submitted_preview_revision_;
        if (dirty_) {
            recreate_swapchain();
        }
        if (offscreen_) {
            image_index_ = 0;
        } else {
            const auto acquired = vkAcquireNextImageKHR(device_, swapchain_,
                                                        blocking_capture ? timeout : 1'000'000ULL,
                                                        acquired_, VK_NULL_HANDLE, &image_index_);
            if (acquired == VK_NOT_READY || acquired == VK_TIMEOUT) {
                frame_wait_reason_ = "Swapchain acquisition pending";
                requestUpdate();
                return;
            }
            if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
                frame_wait_reason_ = "Swapchain recreation pending";
                dirty_             = true;
                requestUpdate();
                return;
            }
            if (acquired != VK_SUBOPTIMAL_KHR) {
                check_vk(acquired, "acquire swapchain image");
            }
        }
        check_vk(vkResetCommandBuffer(command_, 0), "reset command buffer");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(command_, &begin), "begin frame");
        // 显示算法共享目标，选择和缓冲更新发生在上一帧 fence 完成后。
        if (sdf_visible_) {
            sdf_renderer_->startNextFrame();
        } else if (point_visible_) {
            point_renderer_->startNextFrame();
        } else {
            (offline_visible_ ? offline_renderer_ : renderer_)->startNextFrame();
        }
    } catch (const std::exception& error) {
        report_failure(QString::fromUtf8(error.what()));
    }
}

void VulkanViewport::frameReady() {
    if (!failure_.isEmpty()) {
        return;
    }
    if (capture_) {
        record_readback();
    }
    VkImageMemoryBarrier transition{};
    transition.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    transition.srcAccessMask =
        capture_ ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    transition.oldLayout =
        capture_ ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    transition.newLayout = offscreen_ ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    transition.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    transition.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    transition.image               = images_[image_index_];
    transition.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(
        command_,
        capture_ ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &transition);
    check_vk(vkEndCommandBuffer(command_), "end frame");
    check_vk(vkResetFences(device_, 1, &frame_fence_), "reset frame fence");
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo               submit{};
    submit.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount   = offscreen_ ? 0 : 1;
    submit.pWaitSemaphores      = offscreen_ ? nullptr : &acquired_;
    submit.pWaitDstStageMask    = offscreen_ ? nullptr : &wait_stage;
    submit.commandBufferCount   = 1;
    submit.pCommandBuffers      = &command_;
    submit.signalSemaphoreCount = offscreen_ ? 0 : 1;
    submit.pSignalSemaphores    = offscreen_ ? nullptr : &rendered_[image_index_];
    check_vk(vkQueueSubmit(queue_, 1, &submit, frame_fence_), "submit frame");
    submitted_preview_revision_ = preview_revision_;
    frame_wait_reason_          = "Frame submitted";
    if (!offscreen_) {
        VkPresentInfoKHR present{};
        present.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores    = &rendered_[image_index_];
        present.swapchainCount     = 1;
        present.pSwapchains        = &swapchain_;
        present.pImageIndices      = &image_index_;
        vulkanInstance()->presentAboutToBeQueued(this);
        const auto result = vkQueuePresentKHR(queue_, &present);
        vulkanInstance()->presentQueued(this);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            dirty_ = true;
        } else {
            check_vk(result, "present frame");
        }
    }
    if (capture_) {
        finish_readback();
    }
}

void VulkanViewport::record_readback() {
    const VkDeviceSize bytes = VkDeviceSize(extent_.width) * extent_.height * 4;
    VkBufferCreateInfo buffer{};
    buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer.size  = bytes;
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    check_vk(vkCreateBuffer(device_, &buffer, nullptr, &readback_), "create readback buffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, readback_, &requirements);
    VkMemoryAllocateInfo allocation{};
    allocation.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize  = requirements.size;
    allocation.memoryTypeIndex = find_memory_type(physical_, requirements.memoryTypeBits,
                                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check_vk(vkAllocateMemory(device_, &allocation, nullptr, &readback_memory_),
             "allocate readback memory");
    check_vk(vkBindBufferMemory(device_, readback_, readback_memory_, 0), "bind readback memory");
    VkImageMemoryBarrier transition{};
    transition.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    transition.srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    transition.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    transition.oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    transition.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    transition.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    transition.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    transition.image               = images_[image_index_];
    transition.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &transition);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent      = {extent_.width, extent_.height, 1};
    vkCmdCopyImageToBuffer(command_, images_[image_index_], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback_, 1, &copy);
    // fence 等待使拷贝对 host 可见；使用 coherent memory，无需额外 invalidate。
}

void VulkanViewport::finish_readback() {
    check_vk(vkWaitForFences(device_, 1, &frame_fence_, VK_TRUE, 2'000'000'000ULL),
             "wait readback");
    void* mapped = nullptr;
    check_vk(vkMapMemory(device_, readback_memory_, 0, VK_WHOLE_SIZE, 0, &mapped), "map readback");
    captured_ = QImage(static_cast<const uchar*>(mapped), static_cast<int>(extent_.width),
                       static_cast<int>(extent_.height), QImage::Format_RGBA8888)
                    .copy();
    if (color_format_ == VK_FORMAT_B8G8R8A8_SRGB) {
        captured_ = captured_.rgbSwapped();
    }
    vkUnmapMemory(device_, readback_memory_);
    vkDestroyBuffer(device_, readback_, nullptr);
    vkFreeMemory(device_, readback_memory_, nullptr);
    readback_        = VK_NULL_HANDLE;
    readback_memory_ = VK_NULL_HANDLE;
}

QImage VulkanViewport::grab() {
    if (!supportsGrab()) {
        return {};
    }
    captured_ = {};
    capture_  = true;
    render_frame(true);
    capture_ = false;
    return captured_;
}

void VulkanViewport::release_swapchain() {
    if (renderer_) {
        renderer_->releaseSwapChainResources();
    }
    if (offline_renderer_) {
        offline_renderer_->releaseSwapChainResources();
    }
    if (point_renderer_) {
        point_renderer_->releaseSwapChainResources();
    }
    if (sdf_renderer_) {
        sdf_renderer_->releaseSwapChainResources();
    }
    for (auto framebuffer : framebuffers_) {
        vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
    framebuffers_.clear();
    vkDestroyRenderPass(device_, render_pass_, nullptr);
    render_pass_ = VK_NULL_HANDLE;
    vkDestroyImageView(device_, depth_view_, nullptr);
    vkDestroyImage(device_, depth_, nullptr);
    vkFreeMemory(device_, depth_memory_, nullptr);
    depth_view_   = VK_NULL_HANDLE;
    depth_        = VK_NULL_HANDLE;
    depth_memory_ = VK_NULL_HANDLE;
    if (!offscreen_) {
        for (auto view : views_) {
            vkDestroyImageView(device_, view, nullptr);
        }
    }
    for (auto semaphore : rendered_) {
        vkDestroySemaphore(device_, semaphore, nullptr);
    }
    views_.clear();
    rendered_.clear();
    images_.clear();
    if (!offscreen_) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    }
    offscreen_color_.release();
    swapchain_ = VK_NULL_HANDLE;
}

void VulkanViewport::release() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    vkDeviceWaitIdle(device_);
    release_swapchain();
    if (renderer_) {
        renderer_->releaseResources();
        renderer_.reset();
    }
    if (offline_renderer_) {
        offline_renderer_->releaseResources();
        offline_renderer_.reset();
    }
    if (point_renderer_) {
        point_renderer_->releaseResources();
        point_renderer_.reset();
    }
    if (sdf_renderer_) {
        sdf_renderer_->releaseResources();
        sdf_renderer_.reset();
    }
    vkDestroyBuffer(device_, readback_, nullptr);
    vkFreeMemory(device_, readback_memory_, nullptr);
    vkDestroyFence(device_, frame_fence_, nullptr);
    vkDestroySemaphore(device_, acquired_, nullptr);
    vkDestroyCommandPool(device_, command_pool_, nullptr);
    vkDestroyDevice(device_, nullptr);
    device_          = VK_NULL_HANDLE;
    command_pool_    = VK_NULL_HANDLE;
    command_         = VK_NULL_HANDLE;
    frame_fence_     = VK_NULL_HANDLE;
    acquired_        = VK_NULL_HANDLE;
    readback_        = VK_NULL_HANDLE;
    readback_memory_ = VK_NULL_HANDLE;
    surface_         = VK_NULL_HANDLE;
    dirty_           = true;
}

bool VulkanViewport::event(QEvent* event) {
    if (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide ||
        event->type() == QEvent::UngrabMouse) {
        finish_gizmo_drag(event->type() == QEvent::UngrabMouse);
        dragging_split_  = false;
        pressed_buttons_ = Qt::NoButton;
        dragged_         = false;
    }
    // QWidget 容器可以在 C++ 析构前销毁原生 surface；必须先释放关联交换链。
    if (event->type() == QEvent::PlatformSurface) {
        auto* surface_event = static_cast<QPlatformSurfaceEvent*>(event);
        if (surface_event->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
            release();
        }
    }
    return QWindow::event(event);
}

void VulkanViewport::requestUpdate() {
    if (scheduled_ || !failure_.isEmpty()) {
        return;
    }
    scheduled_ = true;
    QTimer::singleShot(16, this, [this] {
        scheduled_ = false;
        render_frame();
    });
}

void VulkanViewport::exposeEvent(QExposeEvent*) {
    if (isExposed()) {
        requestUpdate();
    }
}

void VulkanViewport::resizeEvent(QResizeEvent* event) {
    const auto a = event->oldSize(), b = event->size();
    if (path_reference_ && a.height() > 0 && b.height() > 0 &&
        std::abs(double(a.width()) / a.height() - double(b.width()) / b.height()) > .001) {
        set_reference_image({});
    }

    finish_gizmo_drag(true);
    dirty_ = dirty_ || event->size() != event->oldSize();
    rebuild_gizmo();
    requestUpdate();
}

bool VulkanViewport::isValid() const noexcept {
    return render_pass_ != VK_NULL_HANDLE && failure_.isEmpty();
}

VkDevice VulkanViewport::device() const noexcept {
    return device_;
}

VkPhysicalDevice VulkanViewport::physicalDevice() const noexcept {
    return physical_;
}

const VkPhysicalDeviceProperties* VulkanViewport::physicalDeviceProperties() const noexcept {
    return &properties_;
}

VkRenderPass VulkanViewport::defaultRenderPass() const noexcept {
    return render_pass_;
}

VkCommandBuffer VulkanViewport::currentCommandBuffer() const noexcept {
    return command_;
}

VkFramebuffer VulkanViewport::currentFramebuffer() const {
    return framebuffers_.at(image_index_);
}

QSize VulkanViewport::swapChainImageSize() const noexcept {
    return {static_cast<int>(extent_.width), static_cast<int>(extent_.height)};
}

bool VulkanViewport::supportsGrab() const noexcept {
    return isValid() && can_grab_;
}

void VulkanViewport::report_frame() {
    ++frame_count_;
}

void VulkanViewport::report_swapchain() {
    ++swapchain_count_;
}

int VulkanViewport::rendered_frames() const noexcept {
    return frame_count_;
}

int VulkanViewport::resource_generations() const noexcept {
    return swapchain_count_;
}

const SceneUploadStatistics& VulkanViewport::upload_statistics() const noexcept {
    return upload_statistics_;
}

void VulkanViewport::report_mesh_upload() {
    ++upload_statistics_.mesh_uploads;
}

void VulkanViewport::report_mesh_allocation() {
    ++upload_statistics_.mesh_allocations;
    ++upload_statistics_.index_uploads;
}

void VulkanViewport::report_vertex_buffer_reuse() {
    ++upload_statistics_.vertex_buffer_reuses;
}

void VulkanViewport::report_descriptor_rebuild() {
    ++upload_statistics_.descriptor_rebuilds;
}

void VulkanViewport::report_scene_update(double milliseconds) {
    ++upload_statistics_.scene_updates;
    upload_statistics_.last_update_ms = milliseconds;
}

void VulkanViewport::mark_preview_changed() {
    if (preview_revision_ == completed_preview_revision_) {
        preview_wait_.start();
    }
    ++preview_revision_;
}

qint64 VulkanViewport::preview_wait_ms() const noexcept {
    return preview_revision_ != completed_preview_revision_ && preview_wait_.isValid()
               ? preview_wait_.elapsed()
               : 0;
}

QString VulkanViewport::frame_wait_reason() const {
    return frame_wait_reason_;
}

void VulkanViewport::report_failure(const QString& error) {
    failure_ = error;
    qCritical().noquote() << "Vulkan renderer:" << error;
}

QString VulkanViewport::failure() const {
    return failure_;
}

minirender::Image VulkanViewport::grab_hdr() {
    if (point_visible_) {
        throw std::logic_error("Return to the scene before exporting HDR");
    }
    if (offline_visible_ && offline_image_) {
        return *offline_image_;
    }
    if (!isValid() || !can_render() || !scene_) {
        throw std::logic_error("HDR capture requires a visible scene viewport");
    }
    const auto before = frame_count_;
    auto*      active =
        sdf_visible_ ? static_cast<ViewportRenderer*>(sdf_renderer_.get()) : renderer_.get();
    active->request_hdr_capture();
    render_frame(true);
    if (!failure_.isEmpty() || frame_count_ == before) {
        throw std::runtime_error("HDR frame was not rendered");
    }
    check_vk(vkWaitForFences(device_, 1, &frame_fence_, VK_TRUE, 2'000'000'000ULL),
             "wait HDR capture");
    return active->read_hdr_capture();
}
