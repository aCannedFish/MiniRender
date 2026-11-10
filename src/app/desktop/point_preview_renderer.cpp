#include "point_preview_renderer.hpp"
#include "gpu_shader.hpp"
#include "vulkan_viewport.hpp"

#include <array>
#include <cstddef>

namespace {
// 与 GLSL push constant 布局一致，低于 Vulkan 1.2 的 128 字节保证容量。
struct PreviewPush {
    glm::mat4 clip{1};
    glm::vec4 viewport{0};
    glm::ivec4 mode{0};
};
static_assert(sizeof(PreviewPush) == 96);
} // namespace

PointPreviewRenderer::PointPreviewRenderer(VulkanViewport* viewport) : viewport_(viewport) {}

void PointPreviewRenderer::initResources() {
    device_ = viewport_->device();
    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PreviewPush)};
    VkPipelineLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    check_vk(vkCreatePipelineLayout(device_, &layout, nullptr, &layout_), "point preview layout");
}

VkPipeline PointPreviewRenderer::create_pipeline(bool lines, bool instanced) {
    GpuShader vertex(device_, ":/shaders/point_preview.vert.spv");
    GpuShader fragment(device_, ":/shaders/point_preview.frag.spv");
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    for (auto& stage : stages) {
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.pName = "main";
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex.handle();
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment.handle();
    using Vertex = minirender::PointPreviewVertex;
    const VkVertexInputBindingDescription binding{
        0, sizeof(Vertex), instanced ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
    const std::array<VkVertexInputAttributeDescription, 2> attributes{
        {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
         {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)}}};
    VkPipelineVertexInputStateCreateInfo input{};
    input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    input.vertexBindingDescriptionCount = 1;
    input.pVertexBindingDescriptions = &binding;
    input.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
    input.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology =
        lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
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
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = lines ? VK_FALSE : VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
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
    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = static_cast<std::uint32_t>(stages.size());
    info.pStages = stages.data();
    info.pVertexInputState = &input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &view;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = layout_;
    info.renderPass = viewport_->defaultRenderPass();
    VkPipeline pipeline = VK_NULL_HANDLE;
    check_vk(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline),
             "point preview pipeline");
    return pipeline;
}

void PointPreviewRenderer::initSwapChainResources() {
    points_pipeline_ = create_pipeline(false, true);
    normals_pipeline_ = create_pipeline(true, true);
    boundary_pipeline_ = create_pipeline(true, false);
}

void PointPreviewRenderer::upload() {
    const auto& data = viewport_->point_preview_data();
    if (data == uploaded_) {
        return;
    }
    const auto copy = [&](GpuBuffer& buffer, const auto& values) {
        buffer.release();
        if (!values.empty()) {
            const auto bytes = values.size() * sizeof(values.front());
            buffer.create(device_, viewport_->physicalDevice(), bytes,
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
            buffer.write(values.data(), bytes);
        }
    };
    if (data) {
        copy(points_, data->points);
        copy(normals_, data->normal_samples);
        copy(boundary_, data->boundary_vertices);
    } else {
        points_.release();
        normals_.release();
        boundary_.release();
    }
    uploaded_ = data;
    viewport_->report_point_upload();
}

void PointPreviewRenderer::discard_if_unused() {
    if (!viewport_->point_preview_data() && uploaded_) {
        points_.release();
        normals_.release();
        boundary_.release();
        uploaded_.reset();
    }
}

void PointPreviewRenderer::startNextFrame() {
    upload();
    const auto command = viewport_->currentCommandBuffer();
    const auto size = viewport_->swapChainImageSize();
    const VkExtent2D extent{static_cast<std::uint32_t>(size.width()),
                            static_cast<std::uint32_t>(size.height())};
    std::array<VkClearValue, 2> clear{};
    clear[0].color = {{.08F, .09F, .11F, 1}};
    clear[1].depthStencil = {1, 0};
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = viewport_->defaultRenderPass();
    begin.framebuffer = viewport_->currentFramebuffer();
    begin.renderArea.extent = extent;
    begin.clearValueCount = static_cast<std::uint32_t>(clear.size());
    begin.pClearValues = clear.data();
    vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport view{0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height),
                          0, 1};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(command, 0, 1, &view);
    vkCmdSetScissor(command, 0, 1, &scissor);
    if (uploaded_ && !uploaded_->points.empty()) {
        const auto& camera = viewport_->camera();
        const auto& settings = viewport_->point_preview_settings();
        PreviewPush push;
        push.clip = camera.projection_matrix(static_cast<float>(extent.width) /
                                             static_cast<float>(extent.height)) *
                    camera.view_matrix();
        push.viewport = {1.0F / static_cast<float>(extent.width),
                         1.0F / static_cast<float>(extent.height), settings.point_radius,
                         settings.normal_length};
        push.mode.y = settings.normal_colors && uploaded_->has_normals ? 1 : 0;
        const auto draw = [&](VkPipeline pipeline, const GpuBuffer& buffer, std::int32_t mode,
                              std::uint32_t vertices, std::uint32_t instances) {
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            const VkDeviceSize offset = 0;
            const auto handle = buffer.handle();
            vkCmdBindVertexBuffers(command, 0, 1, &handle, &offset);
            push.mode.x = mode;
            vkCmdPushConstants(command, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push),
                               &push);
            vkCmdDraw(command, vertices, instances, 0, 0);
        };
        draw(points_pipeline_, points_, 0, 6, static_cast<std::uint32_t>(uploaded_->points.size()));
        if (settings.show_normals && !uploaded_->normal_samples.empty()) {
            draw(normals_pipeline_, normals_, 1, 2,
                 static_cast<std::uint32_t>(uploaded_->normal_samples.size()));
        }
        if (settings.show_boundaries && !uploaded_->boundary_vertices.empty()) {
            draw(boundary_pipeline_, boundary_, 2,
                 static_cast<std::uint32_t>(uploaded_->boundary_vertices.size()), 1);
        }
    }
    vkCmdEndRenderPass(command);
    viewport_->report_frame();
    viewport_->frameReady();
    viewport_->requestUpdate();
}

void PointPreviewRenderer::releaseSwapChainResources() {
    vkDestroyPipeline(device_, points_pipeline_, nullptr);
    vkDestroyPipeline(device_, normals_pipeline_, nullptr);
    vkDestroyPipeline(device_, boundary_pipeline_, nullptr);
    points_pipeline_ = normals_pipeline_ = boundary_pipeline_ = VK_NULL_HANDLE;
}

void PointPreviewRenderer::releaseResources() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    releaseSwapChainResources();
    points_.release();
    normals_.release();
    boundary_.release();
    uploaded_.reset();
    vkDestroyPipelineLayout(device_, layout_, nullptr);
    layout_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
}
