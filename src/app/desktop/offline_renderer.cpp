#include "offline_renderer.hpp"
#include "gpu_shader.hpp"
#include "vulkan_viewport.hpp"

#include <algorithm>
#include <array>
#include <cmath>

OfflineRenderer::OfflineRenderer(VulkanViewport* window) : window_(window) {}

void OfflineRenderer::initResources() {
    device_ = window_->device();
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    check_vk(vkCreateSampler(device_, &sampler, nullptr, &sampler_), "offline sampler");
    const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                               VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo descriptor_layout{};
    descriptor_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptor_layout.bindingCount = 1;
    descriptor_layout.pBindings = &binding;
    check_vk(vkCreateDescriptorSetLayout(device_, &descriptor_layout, nullptr, &descriptor_layout_),
             "offline descriptor layout");
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    check_vk(vkCreateDescriptorPool(device_, &pool, nullptr, &descriptor_pool_), "offline pool");
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = descriptor_pool_;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &descriptor_layout_;
    check_vk(vkAllocateDescriptorSets(device_, &allocate, &descriptor_), "offline descriptor");
    const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float)};
    VkPipelineLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &descriptor_layout_;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    check_vk(vkCreatePipelineLayout(device_, &layout, nullptr, &layout_), "offline layout");
}

void OfflineRenderer::initSwapChainResources() {
    GpuShader vertex(device_, ":/shaders/post.vert.spv");
    GpuShader fragment(device_, ":/shaders/offline.frag.spv");
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    for (auto& stage : stages) {
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.pName = "main";
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex.handle();
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment.handle();
    VkPipelineVertexInputStateCreateInfo input{};
    input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    view.viewportCount = view.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    const std::array<VkDynamicState, 2> states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(states.size());
    dynamic.pDynamicStates = states.data();
    VkGraphicsPipelineCreateInfo pipeline{};
    pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline.stageCount = static_cast<std::uint32_t>(stages.size());
    pipeline.pStages = stages.data();
    pipeline.pVertexInputState = &input;
    pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &view;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &multisample;
    pipeline.pDepthStencilState = &depth;
    pipeline.pColorBlendState = &blend;
    pipeline.pDynamicState = &dynamic;
    pipeline.layout = layout_;
    pipeline.renderPass = window_->defaultRenderPass();
    check_vk(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr, &pipeline_),
             "offline pipeline");
}

void OfflineRenderer::upload(VkCommandBuffer command) {
    const auto& image = window_->offline_image();
    if (!image || image == displayed_) {
        return;
    }
    const VkExtent2D size{static_cast<std::uint32_t>(image->width()),
                          static_cast<std::uint32_t>(image->height())};
    if (size.width != extent_.width || size.height != extent_.height) {
        texture_.create(device_, window_->physicalDevice(), size, VK_FORMAT_R32G32B32A32_SFLOAT,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT);
        staging_.create(device_, window_->physicalDevice(),
                        VkDeviceSize(size.width) * size.height * sizeof(glm::vec4),
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        extent_ = size;
        uploaded_ = false;
        const VkDescriptorImageInfo info{sampler_, texture_.view(),
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptor_;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &info;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }
    pixels_.resize(image->width() * image->height());
    for (std::size_t y = 0; y < image->height(); ++y) {
        for (std::size_t x = 0; x < image->width(); ++x) {
            pixels_[y * image->width() + x] = glm::vec4(image->at(x, y), 1);
        }
    }
    staging_.write(pixels_.data(), pixels_.size() * sizeof(glm::vec4));
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.oldLayout =
        uploaded_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcAccessMask = uploaded_ ? VK_ACCESS_SHADER_READ_BIT : 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.image = texture_.image();
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command,
                         uploaded_ ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                   : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {size.width, size.height, 1};
    vkCmdCopyBufferToImage(command, staging_.handle(), texture_.image(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
    uploaded_ = true;
    displayed_ = image;
}

void OfflineRenderer::startNextFrame() {
    const auto command = window_->currentCommandBuffer();
    upload(command);
    const auto size = window_->swapChainImageSize();
    std::array<VkClearValue, 2> clear{};
    clear[0].color = {{0, 0, 0, 1}};
    clear[1].depthStencil = {1, 0};
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = window_->defaultRenderPass();
    begin.framebuffer = window_->currentFramebuffer();
    begin.renderArea.extent = {static_cast<std::uint32_t>(size.width()),
                               static_cast<std::uint32_t>(size.height())};
    begin.clearValueCount = static_cast<std::uint32_t>(clear.size());
    begin.pClearValues = clear.data();
    vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    if (window_->offline_image() && displayed_) {
        const double scale = std::min(static_cast<double>(size.width()) / extent_.width,
                                      static_cast<double>(size.height()) / extent_.height);
        const auto width =
            static_cast<std::uint32_t>(std::max(1.0, std::floor(extent_.width * scale)));
        const auto height =
            static_cast<std::uint32_t>(std::max(1.0, std::floor(extent_.height * scale)));
        const VkRect2D rect{{(size.width() - static_cast<int>(width)) / 2,
                             (size.height() - static_cast<int>(height)) / 2},
                            {width, height}};
        const VkViewport view{static_cast<float>(rect.offset.x),
                              static_cast<float>(rect.offset.y),
                              static_cast<float>(width),
                              static_cast<float>(height),
                              0,
                              1};
        vkCmdSetViewport(command, 0, 1, &view);
        vkCmdSetScissor(command, 0, 1, &rect);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1,
                                &descriptor_, 0, nullptr);
        const auto exposure = window_->exposure();
        vkCmdPushConstants(command, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float),
                           &exposure);
        vkCmdDraw(command, 3, 1, 0, 0);
    }
    vkCmdEndRenderPass(command);
    window_->report_frame();
    window_->frameReady();
    window_->requestUpdate();
}

void OfflineRenderer::releaseSwapChainResources() {
    vkDestroyPipeline(device_, pipeline_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
}
void OfflineRenderer::releaseResources() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    releaseSwapChainResources();
    texture_.release();
    staging_.release();
    vkDestroyPipelineLayout(device_, layout_, nullptr);
    vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    vkDestroyDescriptorSetLayout(device_, descriptor_layout_, nullptr);
    vkDestroySampler(device_, sampler_, nullptr);
    device_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    descriptor_pool_ = VK_NULL_HANDLE;
    descriptor_layout_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    displayed_.reset();
    pixels_.clear();
    extent_ = {};
    uploaded_ = false;
}
