// 线性图像到屏幕显示的转换、FXAA 和预览轮廓。
// 本模块接口见 post_process.hpp。

#include "post_process.hpp"
#include "gpu_shader.hpp"
#include "vulkan_utils.hpp"

#include <array>

PostProcess::~PostProcess() {
    release();
}

void PostProcess::create(VulkanViewport& viewport, const HdrTargets& targets,
                         VkBuffer uniform_buffer, VkDeviceSize uniform_size, VkImageView hdr,
                         VkImageView raw_ao, VkImageView filtered_ao) {
    device_ = viewport.device();
    const std::array<ScreenInput, 3> outline_inputs{
        ScreenInput{hdr}, ScreenInput{targets.normal_view()},
        ScreenInput{targets.depth_view(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}};
    outline_.create(viewport, VK_FORMAT_R32G32B32A32_SFLOAT, ":/shaders/preview_outline.frag.spv",
                    uniform_buffer, uniform_size, outline_inputs);
    VkSamplerCreateInfo sampler{};
    sampler.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter    = VK_FILTER_NEAREST;
    sampler.minFilter    = VK_FILTER_NEAREST;
    sampler.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    check_vk(vkCreateSampler(device_, &sampler, nullptr, &sampler_), "create post sampler");
    std::array<VkDescriptorSetLayoutBinding, 8> bindings{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    for (std::uint32_t i = 1; i < bindings.size(); ++i) {
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    }
    VkDescriptorSetLayoutCreateInfo descriptor_layout{};
    descriptor_layout.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptor_layout.bindingCount = static_cast<std::uint32_t>(bindings.size());
    descriptor_layout.pBindings    = bindings.data();
    check_vk(vkCreateDescriptorSetLayout(device_, &descriptor_layout, nullptr, &descriptor_layout_),
             "post descriptor layout");
    const std::array<VkDescriptorPoolSize, 2> sizes{
        {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 14}}};
    VkDescriptorPoolCreateInfo pool{};
    pool.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets       = 2;
    pool.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    pool.pPoolSizes    = sizes.data();
    check_vk(vkCreateDescriptorPool(device_, &pool, nullptr, &descriptor_pool_),
             "post descriptor pool");
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool     = descriptor_pool_;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts        = &descriptor_layout_;
    check_vk(vkAllocateDescriptorSets(device_, &allocate, &descriptor_), "post descriptor set");
    VkDescriptorBufferInfo                     buffer{uniform_buffer, 0, uniform_size};
    const std::array<VkDescriptorImageInfo, 7> images{
        {{sampler_, hdr, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
         {sampler_, targets.normal_view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
         {sampler_, targets.depth_view(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
         {sampler_, targets.wire_view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
         {sampler_, raw_ao, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
         {sampler_, filtered_ao, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
         {sampler_, outline_.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}};
    uniform_info_ = buffer;
    for (std::size_t i = 0; i < images.size(); ++i) {
        inputs_[i] = {images[i].imageView, images[i].imageLayout};
    }
    std::array<VkWriteDescriptorSet, 8> writes{};
    for (std::uint32_t i = 0; i < writes.size(); ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = descriptor_;
        writes[i].dstBinding      = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType =
            i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        if (i == 0) {
            writes[i].pBufferInfo = &buffer;
        } else {
            writes[i].pImageInfo = &images[i - 1];
        }
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0,
                           nullptr);
    VkPipelineLayoutCreateInfo layout{};
    layout.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = 1;
    layout.pSetLayouts    = &descriptor_layout_;
    const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(std::int32_t)};
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges    = &push;
    check_vk(vkCreatePipelineLayout(device_, &layout, nullptr, &layout_), "post pipeline layout");
    GpuShader                                      vertex(device_, ":/shaders/post.vert.spv");
    GpuShader                                      fragment(device_, ":/shaders/post.frag.spv");
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    for (auto& stage : stages) {
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.pName = "main";
    }
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex.handle();
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment.handle();
    VkPipelineVertexInputStateCreateInfo input{};
    input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo view{};
    view.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    view.viewportCount = 1;
    view.scissorCount  = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode    = VK_CULL_MODE_NONE;
    raster.lineWidth   = 1;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments    = &attachment;
    const std::array<VkDynamicState, 2> dynamic_states{VK_DYNAMIC_STATE_VIEWPORT,
                                                       VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo    dynamic{};
    dynamic.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
    dynamic.pDynamicStates    = dynamic_states.data();
    VkGraphicsPipelineCreateInfo pipeline{};
    pipeline.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline.stageCount          = static_cast<std::uint32_t>(stages.size());
    pipeline.pStages             = stages.data();
    pipeline.pVertexInputState   = &input;
    pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState      = &view;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState   = &multisample;
    pipeline.pDepthStencilState  = &depth;
    pipeline.pColorBlendState    = &blend;
    pipeline.pDynamicState       = &dynamic;
    pipeline.layout              = layout_;
    pipeline.renderPass          = viewport.defaultRenderPass();
    check_vk(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr, &pipeline_),
             "post pipeline");
}

void PostProcess::draw(VulkanViewport& viewport, const std::function<void()>& overlay) {
    update_outline(viewport, viewport.view_mode());
    present(viewport, descriptor_, 0, overlay);
}

void PostProcess::update_outline(VulkanViewport& viewport, int mode) {
    // 首帧使已绑定附件的布局有效；仅在原色轮廓启用时持续计算。
    if (!outline_initialized_ ||
        (mode == VulkanViewport::base_color_preview_mode && viewport.preview_outline() &&
         viewport.preview_outline_strength() > 0)) {
        outline_.draw(viewport);
        outline_initialized_ = true;
    }
}

void PostProcess::prepare_comparison(VulkanViewport& viewport) {
    if (comparison_set_ != VK_NULL_HANDLE) {
        return;
    }
    for (auto& side : sides_) {
        side.create(viewport, VK_FORMAT_R16G16B16A16_SFLOAT, ":/shaders/post.frag.spv",
                    uniform_info_.buffer, uniform_info_.range, inputs_, ":/shaders/post.vert.spv");
    }
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool     = descriptor_pool_;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts        = &descriptor_layout_;
    check_vk(vkAllocateDescriptorSets(device_, &allocate, &comparison_set_),
             "allocate comparison set");
    // post.frag 的两个分支共用静态描述符布局。复制原套，再将前两张图绑定为缓存。
    std::array<VkCopyDescriptorSet, 8> copies{};
    for (std::uint32_t i = 0; i < copies.size(); ++i) {
        auto& copy      = copies[i];
        copy.sType      = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET;
        copy.srcSet     = descriptor_;
        copy.dstSet     = comparison_set_;
        copy.srcBinding = copy.dstBinding = i;
        copy.descriptorCount              = 1;
    }
    vkUpdateDescriptorSets(device_, 0, nullptr, static_cast<std::uint32_t>(copies.size()),
                           copies.data());
    std::array<VkDescriptorImageInfo, 2> images{};
    std::array<VkWriteDescriptorSet, 2>  writes{};
    for (std::uint32_t i = 0; i < 2; ++i) {
        images[i]        = {sampler_, sides_[i].view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        auto& write      = writes[i];
        write.sType      = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet     = comparison_set_;
        write.dstBinding = i + 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo      = &images[i];
    }
    vkUpdateDescriptorSets(device_, 2, writes.data(), 0, nullptr);
}

void PostProcess::store_side(VulkanViewport& viewport, std::size_t side, int mode) {
    update_outline(viewport, mode);
    sides_.at(side).draw(viewport);
}

void PostProcess::present_comparison(VulkanViewport&              viewport,
                                     const std::function<void()>& overlay) {
    const int width = viewport.swapChainImageSize().width();
    const int split =
        viewport.export_side() >= 0
            ? -1 - viewport.export_side()
            : 1 + static_cast<int>(viewport.split_position() * static_cast<float>(width));
    present(viewport, comparison_set_, split, overlay);
}

void PostProcess::present(VulkanViewport& viewport, VkDescriptorSet set, int split,
                          const std::function<void()>& overlay) {
    const auto                  size = viewport.swapChainImageSize();
    const VkExtent2D            extent{static_cast<std::uint32_t>(size.width()),
                                       static_cast<std::uint32_t>(size.height())};
    std::array<VkClearValue, 2> clear{};
    clear[0].color        = {{0, 0, 0, 1}};
    clear[1].depthStencil = {1, 0};
    VkRenderPassBeginInfo begin{};
    begin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass        = viewport.defaultRenderPass();
    begin.framebuffer       = viewport.currentFramebuffer();
    begin.renderArea.extent = extent;
    begin.clearValueCount   = static_cast<std::uint32_t>(clear.size());
    begin.pClearValues      = clear.data();
    const auto command      = viewport.currentCommandBuffer();
    vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport screen{
        0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height), 0, 1};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(command, 0, 1, &screen);
    vkCmdSetScissor(command, 0, 1, &scissor);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 0,
                            nullptr);
    vkCmdPushConstants(command, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(split), &split);
    vkCmdDraw(command, 3, 1, 0, 0);
    if (overlay) {
        overlay();
    }
    vkCmdEndRenderPass(command);
}

void PostProcess::release() {
    comparison_set_ = VK_NULL_HANDLE;
    for (auto& side : sides_) {
        side.release();
    }
    outline_.release();
    outline_initialized_ = false;
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    vkDestroyPipeline(device_, pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, layout_, nullptr);
    vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    vkDestroyDescriptorSetLayout(device_, descriptor_layout_, nullptr);
    vkDestroySampler(device_, sampler_, nullptr);
    device_            = VK_NULL_HANDLE;
    pipeline_          = VK_NULL_HANDLE;
    layout_            = VK_NULL_HANDLE;
    descriptor_pool_   = VK_NULL_HANDLE;
    descriptor_layout_ = VK_NULL_HANDLE;
    sampler_           = VK_NULL_HANDLE;
}
