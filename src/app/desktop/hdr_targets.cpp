#include "hdr_targets.hpp"

#include <array>
#include <stdexcept>
#include <vector>

HdrTargets::~HdrTargets() {
    release();
}

void HdrTargets::create(VulkanViewport& viewport) {
    device_ = viewport.device();
    physical_ = viewport.physicalDevice();
    const auto size = viewport.swapChainImageSize();
    extent_ = {static_cast<std::uint32_t>(size.width()), static_cast<std::uint32_t>(size.height())};
    color_.create(device_, physical_, extent_, VK_FORMAT_R32G32B32A32_SFLOAT,
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                  VK_IMAGE_ASPECT_COLOR_BIT);
    normal_.create(device_, physical_, extent_, VK_FORMAT_R16G16B16A16_SFLOAT,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT);
    depth_.create(device_, physical_, extent_, VK_FORMAT_D32_SFLOAT,
                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  VK_IMAGE_ASPECT_DEPTH_BIT);
    std::array<VkAttachmentDescription, 3> attachments{};
    for (auto& attachment : attachments) {
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    attachments[0].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attachments[1].format = VK_FORMAT_R16G16B16A16_SFLOAT;
    attachments[2].format = VK_FORMAT_D32_SFLOAT;
    attachments[2].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    const std::array<VkAttachmentReference, 2> colors{
        {{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
         {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}}};
    const VkAttachmentReference depth{2, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = static_cast<std::uint32_t>(colors.size());
    subpass.pColorAttachments = colors.data();
    subpass.pDepthStencilAttachment = &depth;
    std::array<VkSubpassDependency, 2> dependencies{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pass.attachmentCount = static_cast<std::uint32_t>(attachments.size());
    pass.pAttachments = attachments.data();
    pass.subpassCount = 1;
    pass.pSubpasses = &subpass;
    pass.dependencyCount = static_cast<std::uint32_t>(dependencies.size());
    pass.pDependencies = dependencies.data();
    check_vk(vkCreateRenderPass(device_, &pass, nullptr, &pass_), "create HDR pass");
    const std::array<VkImageView, 3> views{color_.view(), normal_.view(), depth_.view()};
    VkFramebufferCreateInfo framebuffer{};
    framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer.renderPass = pass_;
    framebuffer.attachmentCount = static_cast<std::uint32_t>(views.size());
    framebuffer.pAttachments = views.data();
    framebuffer.width = extent_.width;
    framebuffer.height = extent_.height;
    framebuffer.layers = 1;
    check_vk(vkCreateFramebuffer(device_, &framebuffer, nullptr, &framebuffer_),
             "create HDR framebuffer");
}

void HdrTargets::begin(VkCommandBuffer command) {
    std::array<VkClearValue, 3> clear{};
    clear[0].color = {{0.012F, 0.018F, 0.028F, 1}};
    clear[1].color = {{0, 0, 0, 0}};
    clear[2].depthStencil = {1, 0};
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = pass_;
    begin.framebuffer = framebuffer_;
    begin.renderArea.extent = extent_;
    begin.clearValueCount = static_cast<std::uint32_t>(clear.size());
    begin.pClearValues = clear.data();
    vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
}

void HdrTargets::record_readback(VkCommandBuffer command) {
    const VkDeviceSize bytes = VkDeviceSize(extent_.width) * extent_.height * 4 * sizeof(float);
    readback_.create(device_, physical_, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = color_.image();
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {extent_.width, extent_.height, 1};
    vkCmdCopyImageToBuffer(command, color_.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback_.handle(), 1, &copy);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
    captured_ = true;
}

minirender::Image HdrTargets::readback() {
    if (!captured_) {
        throw std::logic_error("No HDR capture was recorded");
    }
    std::vector<float> rgba(std::size_t(extent_.width) * extent_.height * 4);
    readback_.read(rgba.data(), rgba.size() * sizeof(float));
    minirender::Image image(extent_.width, extent_.height);
    for (std::size_t y = 0; y < extent_.height; ++y) {
        for (std::size_t x = 0; x < extent_.width; ++x) {
            const auto index = (y * extent_.width + x) * 4;
            image.at(x, y) = {rgba[index], rgba[index + 1], rgba[index + 2]};
        }
    }
    return image;
}

void HdrTargets::release() {
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device_, framebuffer_, nullptr);
        vkDestroyRenderPass(device_, pass_, nullptr);
    }
    framebuffer_ = VK_NULL_HANDLE;
    pass_ = VK_NULL_HANDLE;
    readback_.release();
    color_.release();
    normal_.release();
    depth_.release();
    device_ = VK_NULL_HANDLE;
    captured_ = false;
}
VkRenderPass HdrTargets::render_pass() const noexcept {
    return pass_;
}
VkImageView HdrTargets::color_view() const noexcept {
    return color_.view();
}
VkImageView HdrTargets::normal_view() const noexcept {
    return normal_.view();
}
VkImageView HdrTargets::depth_view() const noexcept {
    return depth_.view();
}
