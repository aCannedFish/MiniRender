#include "gizmo_renderer.hpp"
#include "gpu_shader.hpp"
#include "vulkan_viewport.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace {

constexpr std::size_t maximum_vertices = 4096;
const glm::vec4 outline(0.035F, 0.04F, 0.055F, 1);

glm::vec4 handle_color(minirender::GizmoHandle handle, minirender::GizmoHandle highlighted) {
    using Handle = minirender::GizmoHandle;
    if (handle == highlighted) {
        return {1, 0.84F, 0.2F, 1};
    }
    switch (handle) {
    case Handle::x:
        return {0.96F, 0.24F, 0.2F, 1};
    case Handle::y:
        return {0.3F, 0.88F, 0.38F, 1};
    case Handle::z:
        return {0.28F, 0.55F, 1, 1};
    default:
        return {0.94F, 0.95F, 0.98F, 1};
    }
}

} // namespace

GizmoRenderer::~GizmoRenderer() {
    release();
}

void GizmoRenderer::create(VulkanViewport& viewport) {
    device_ = viewport.device();
    static_assert(sizeof(Vertex) == 24);
    buffer_.create(device_, viewport.physicalDevice(), maximum_vertices * sizeof(Vertex),
                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    vertices_.reserve(maximum_vertices);
    VkPipelineLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    check_vk(vkCreatePipelineLayout(device_, &layout, nullptr, &layout_),
             "create gizmo pipeline layout");
    GpuShader vertex(device_, ":/shaders/gizmo.vert.spv");
    GpuShader fragment(device_, ":/shaders/gizmo.frag.spv");
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    for (auto& stage : stages) {
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.pName = "main";
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex.handle();
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment.handle();
    const VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const std::array<VkVertexInputAttributeDescription, 2> attributes{
        {{0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, position)},
         {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)}}};
    VkPipelineVertexInputStateCreateInfo input{};
    input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    input.vertexBindingDescriptionCount = 1;
    input.pVertexBindingDescriptions = &binding;
    input.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
    input.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo screen{};
    screen.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    screen.viewportCount = 1;
    screen.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
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
    const std::array<VkDynamicState, 2> dynamic_states{VK_DYNAMIC_STATE_VIEWPORT,
                                                       VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
    dynamic.pDynamicStates = dynamic_states.data();
    VkGraphicsPipelineCreateInfo pipeline{};
    pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline.stageCount = static_cast<std::uint32_t>(stages.size());
    pipeline.pStages = stages.data();
    pipeline.pVertexInputState = &input;
    pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &screen;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &multisample;
    pipeline.pDepthStencilState = &depth;
    pipeline.pColorBlendState = &blend;
    pipeline.pDynamicState = &dynamic;
    pipeline.layout = layout_;
    pipeline.renderPass = viewport.defaultRenderPass();
    check_vk(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr, &pipeline_),
             "create gizmo pipeline");
}

void GizmoRenderer::triangle(glm::dvec2 a, glm::dvec2 b, glm::dvec2 c, glm::vec4 color,
                             glm::dvec2 size) {
    for (const auto p : {a, b, c}) {
        vertices_.push_back({glm::vec2(p / size * 2.0 - 1.0), color});
    }
}

void GizmoRenderer::stroke(glm::dvec2 a, glm::dvec2 b, double width, glm::vec4 color,
                           glm::dvec2 size) {
    const auto direction = b - a;
    if (glm::length(direction) < 1e-6) {
        return;
    }
    const auto side =
        glm::dvec2(-direction.y, direction.x) * (width / (2 * glm::length(direction)));
    triangle(a - side, a + side, b + side, color, size);
    triangle(a - side, b + side, b - side, color, size);
}

void GizmoRenderer::draw(VulkanViewport& viewport, const minirender::GizmoLayout& layout,
                         minirender::GizmoHandle highlighted) {
    if (!layout.visible || viewport.offline_visible()) {
        return;
    }
    const glm::dvec2 size(viewport.width(), viewport.height());
    if (size.x <= 0 || size.y <= 0) {
        return;
    }
    vertices_.clear();
    auto segments = layout.segments;
    std::stable_sort(segments.begin(), segments.end(),
                     [](const auto& a, const auto& b) { return a.depth > b.depth; });
    for (const auto& segment : segments) {
        stroke(segment.a, segment.b, 6, outline, size);
    }
    for (const auto& segment : segments) {
        stroke(segment.a, segment.b, 3, handle_color(segment.handle, highlighted), size);
        if (layout.mode == minirender::GizmoMode::translate) {
            const auto direction = glm::normalize(segment.b - segment.a);
            const glm::dvec2 side(-direction.y, direction.x);
            triangle(segment.b + 2.0 * direction, segment.b - 13.0 * direction + 7.0 * side,
                     segment.b - 13.0 * direction - 7.0 * side, outline, size);
            triangle(segment.b, segment.b - 10.0 * direction + 4.5 * side,
                     segment.b - 10.0 * direction - 4.5 * side,
                     handle_color(segment.handle, highlighted), size);
        }
    }
    if (layout.mode == minirender::GizmoMode::translate) {
        for (const double radius : {7.0, 4.5}) {
            const auto color = radius == 7.0
                                   ? outline
                                   : handle_color(minirender::GizmoHandle::screen, highlighted);
            for (int i = 0; i < 16; ++i) {
                const auto a = i * 2 * std::numbers::pi / 16;
                const auto b = (i + 1) * 2 * std::numbers::pi / 16;
                triangle(
                    layout.center, layout.center + radius * glm::dvec2(std::cos(a), std::sin(a)),
                    layout.center + radius * glm::dvec2(std::cos(b), std::sin(b)), color, size);
            }
        }
    }
    if (vertices_.size() > maximum_vertices) {
        throw std::length_error("Gizmo vertex capacity exceeded");
    }
    if (!vertices_.empty()) {
        buffer_.write(vertices_.data(), vertices_.size() * sizeof(Vertex));
        const auto command = viewport.currentCommandBuffer();
        const auto buffer = buffer_.handle();
        const VkDeviceSize offset = 0;
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdBindVertexBuffers(command, 0, 1, &buffer, &offset);
        vkCmdDraw(command, static_cast<std::uint32_t>(vertices_.size()), 1, 0, 0);
    }
}

void GizmoRenderer::release() {
    if (device_ != VK_NULL_HANDLE) {
        buffer_.release();
        vkDestroyPipeline(device_, pipeline_, nullptr);
        vkDestroyPipelineLayout(device_, layout_, nullptr);
    }
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
}
