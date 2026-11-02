#include "shadow_map.hpp"
#include "gpu_buffer.hpp"

#include <stdexcept>

ShadowMap::~ShadowMap() {
    release();
}

void ShadowMap::create(VulkanViewport& viewport) {
    device_ = viewport.device();
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(viewport.physicalDevice(), VK_FORMAT_D32_SFLOAT,
                                        &properties);
    const auto required =
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if ((properties.optimalTilingFeatures & required) != required) {
        throw std::runtime_error("D32 sampled depth images are unavailable");
    }
    VkImageCreateInfo image{};
    image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = VK_FORMAT_D32_SFLOAT;
    image.extent = {resolution, resolution, 1};
    image.mipLevels = 1;
    image.arrayLayers = layers;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    check_vk(vkCreateImage(device_, &image, nullptr, &image_), "create shadow array");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, image_, &requirements);
    VkPhysicalDeviceMemoryProperties memory_properties{};
    vkGetPhysicalDeviceMemoryProperties(viewport.physicalDevice(), &memory_properties);
    std::uint32_t memory_type = memory_properties.memoryTypeCount;
    for (std::uint32_t index = 0; index < memory_properties.memoryTypeCount; ++index) {
        if ((requirements.memoryTypeBits & (1U << index)) != 0 &&
            (memory_properties.memoryTypes[index].propertyFlags &
             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
            memory_type = index;
            break;
        }
    }
    if (memory_type == memory_properties.memoryTypeCount) {
        throw std::runtime_error("No shadow memory type");
    }
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = memory_type;
    check_vk(vkAllocateMemory(device_, &allocate, nullptr, &memory_), "allocate shadow map");
    check_vk(vkBindImageMemory(device_, image_, memory_, 0), "bind shadow map");
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = image_;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view.format = VK_FORMAT_D32_SFLOAT;
    view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, layers};
    check_vk(vkCreateImageView(device_, &view, nullptr, &array_view_), "create shadow array view");
    for (std::uint32_t layer = 0; layer < layers; ++layer) {
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.subresourceRange.baseArrayLayer = layer;
        view.subresourceRange.layerCount = 1;
        check_vk(vkCreateImageView(device_, &view, nullptr, &views_[layer]),
                 "create shadow layer view");
    }
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_NEAREST;
    sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    // 在 shader 中显式比较深度，避免依赖 portability mutableComparisonSamplers。
    sampler.compareEnable = VK_FALSE;
    sampler.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    check_vk(vkCreateSampler(device_, &sampler, nullptr, &sampler_), "create shadow sampler");

    VkAttachmentDescription attachment{};
    attachment.format = VK_FORMAT_D32_SFLOAT;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    const VkAttachmentReference depth{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &depth;
    std::array<VkSubpassDependency, 2> dependencies{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask =
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pass.attachmentCount = 1;
    pass.pAttachments = &attachment;
    pass.subpassCount = 1;
    pass.pSubpasses = &subpass;
    pass.dependencyCount = static_cast<std::uint32_t>(dependencies.size());
    pass.pDependencies = dependencies.data();
    check_vk(vkCreateRenderPass(device_, &pass, nullptr, &pass_), "create shadow pass");
    for (std::uint32_t layer = 0; layer < layers; ++layer) {
        VkFramebufferCreateInfo framebuffer{};
        framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer.renderPass = pass_;
        framebuffer.attachmentCount = 1;
        framebuffer.pAttachments = &views_[layer];
        framebuffer.width = resolution;
        framebuffer.height = resolution;
        framebuffer.layers = 1;
        check_vk(vkCreateFramebuffer(device_, &framebuffer, nullptr, &framebuffers_[layer]),
                 "create shadow framebuffer");
    }
}

void ShadowMap::prepare(VkCommandBuffer command) {
    if (initialized_) {
        return;
    }
    // 即便当前没有方向光，也初始化所有层的布局；descriptor 不能引用未转换的子资源。
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image_;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, layers};
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
    initialized_ = true;
}

void ShadowMap::begin(VkCommandBuffer command, std::uint32_t layer) {
    const VkClearValue clear{.depthStencil = {1, 0}};
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = pass_;
    begin.framebuffer = framebuffers_.at(layer);
    begin.renderArea.extent = {resolution, resolution};
    begin.clearValueCount = 1;
    begin.pClearValues = &clear;
    vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
}

void ShadowMap::release() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    for (auto framebuffer : framebuffers_) {
        vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
    vkDestroyRenderPass(device_, pass_, nullptr);
    vkDestroySampler(device_, sampler_, nullptr);
    for (auto view : views_) {
        vkDestroyImageView(device_, view, nullptr);
    }
    vkDestroyImageView(device_, array_view_, nullptr);
    vkDestroyImage(device_, image_, nullptr);
    vkFreeMemory(device_, memory_, nullptr);
    device_ = VK_NULL_HANDLE;
    image_ = VK_NULL_HANDLE;
    memory_ = VK_NULL_HANDLE;
    array_view_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    pass_ = VK_NULL_HANDLE;
    views_.fill(VK_NULL_HANDLE);
    framebuffers_.fill(VK_NULL_HANDLE);
    initialized_ = false;
}

VkRenderPass ShadowMap::render_pass() const noexcept {
    return pass_;
}
VkDescriptorImageInfo ShadowMap::descriptor() const noexcept {
    return {sampler_, array_view_, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
}
