// 创建通道及管线，按交换链、设备的生命周期释放对应资源。

#include "gpu_shader.hpp"
#include "scene_gpu_layout.hpp"
#include "scene_renderer.hpp"
#include "vulkan_utils.hpp"
#include <array>

using namespace scene_gpu;
using Vertex = minirender::RasterVertex;

void SceneRenderer::initResources() {
    device_ = window_->device();
    uniforms_.create(device_, window_->physicalDevice(), sizeof(FrameUniform),
                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    shadow_map_.create(*window_);
    ibl_.create(*window_);
    const minirender::Image empty_reference(1, 1);
    reference_texture_.create(*window_, {&empty_reference, 1});
    rectangles_.create(*window_);
    std::array<VkDescriptorSetLayoutBinding, 15> bindings{};
    auto&                                        binding = bindings[0];
    binding.binding                                      = 0;
    binding.descriptorType                               = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount                              = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    bindings[1]  = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                    nullptr};
    bindings[2]  = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                    nullptr};
    bindings[3]  = {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    for (std::uint32_t i = 4; i < bindings.size(); ++i) {
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    }
    bindings[10] = {10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                    nullptr};
    bindings[11] = {11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[13] = {13, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    layout.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layout.pBindings    = bindings.data();
    check_vk(vkCreateDescriptorSetLayout(device_, &layout, nullptr, &descriptor_layout_),
             "scene descriptor layout");
    minirender::Image white(1, 1);
    white.at(0, 0) = glm::vec3(1);
    white_texture_ = std::make_shared<const minirender::Texture>(std::move(white));
    const VkPushConstantRange  push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                    sizeof(DrawIndex)};
    VkPipelineLayoutCreateInfo pipeline_layout{};
    pipeline_layout.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout.pushConstantRangeCount = 1;
    pipeline_layout.pPushConstantRanges    = &push;
    pipeline_layout.setLayoutCount         = 1;
    pipeline_layout.pSetLayouts            = &descriptor_layout_;
    check_vk(vkCreatePipelineLayout(device_, &pipeline_layout, nullptr, &pipeline_layout_),
             "scene pipeline layout");
}

void SceneRenderer::initSwapChainResources() {
    shadow_depth_ready_ = false;
    histories_          = {};
    comparison_keys_.fill({0, -1});
    targets_.create(*window_);
    for (std::size_t side = 0; side < surface_pipelines_.size(); ++side) {
        const auto cull  = side == 0 ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        const auto front = side == 2 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        surface_pipelines_[side] = create_pipeline(Pass::surface, cull, front);
        shadow_pipelines_[side]  = create_pipeline(Pass::shadow, cull, front);
    }
    wire_pipeline_       = create_pipeline(Pass::wire);
    background_pipeline_ = create_pipeline(Pass::background);
    ssao_.create(*window_, targets_, uniforms_.handle(), sizeof(FrameUniform),
                 reference_texture_.descriptor().imageView);
    post_process_.create(*window_, targets_, uniforms_.handle(), sizeof(FrameUniform),
                         ssao_.hdr_view(), ssao_.raw_view(), ssao_.filtered_view());
    gizmo_.create(*window_);
    window_->report_swapchain();
}

VkPipeline SceneRenderer::create_pipeline(Pass pass, VkCullModeFlags cull, VkFrontFace front) {
    const bool      shadow     = pass == Pass::shadow;
    const bool      wire       = pass == Pass::wire;
    const bool      background = pass == Pass::background;
    const GpuShader vertex(device_, background ? ":/shaders/screen.vert.spv"
                                               : (shadow ? ":/shaders/shadow.vert.spv"
                                                         : (wire ? ":/shaders/wire.vert.spv"
                                                                 : ":/shaders/scene.vert.spv")));
    std::unique_ptr<GpuShader> fragment;
    if (!shadow) {
        fragment = std::make_unique<GpuShader>(
            device_, background ? ":/shaders/environment.frag.spv"
                                : (wire ? ":/shaders/wire.frag.spv" : ":/shaders/scene.frag.spv"));
    }
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    for (auto& stage : stages) {
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.pName = "main";
    }
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex.handle();
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment ? fragment->handle() : VK_NULL_HANDLE;
    const VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    std::array<VkVertexInputAttributeDescription, 5> attributes{
        {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
         {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
         {5, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
         {6, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, tangent_u)},
         {7, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, tangent_v)}}};
    if (wire) {
        attributes[1].offset = offsetof(Vertex, geometric_normal);
    }
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = background ? 0 : 1;
    vertex_input.pVertexBindingDescriptions    = &binding;
    vertex_input.vertexAttributeDescriptionCount =
        background ? 0 : (shadow ? 1 : (wire ? 2 : static_cast<std::uint32_t>(attributes.size())));
    vertex_input.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology =
        wire ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount  = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode             = VK_POLYGON_MODE_FILL;
    raster.cullMode                = cull;
    raster.frontFace               = front;
    raster.lineWidth               = 1;
    raster.depthBiasEnable         = shadow;
    raster.depthBiasConstantFactor = 1.25F;
    raster.depthBiasSlopeFactor    = 1.75F;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable  = !background;
    depth.depthWriteEnable = !wire && !background;
    depth.depthCompareOp   = wire ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_LESS;
    std::array<VkPipelineColorBlendAttachmentState, 4> attachments{};
    for (auto& attachment : attachments) {
        attachment.colorWriteMask = 0xF;
    }
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount =
        shadow ? 0 : (wire ? 1 : static_cast<std::uint32_t>(attachments.size()));
    blend.pAttachments = attachments.data();
    const std::array<VkDynamicState, 2> states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo    dynamic{};
    dynamic.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(states.size());
    dynamic.pDynamicStates    = states.data();
    VkGraphicsPipelineCreateInfo pipeline{};
    pipeline.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline.stageCount          = shadow ? 1 : static_cast<std::uint32_t>(stages.size());
    pipeline.pStages             = stages.data();
    pipeline.pVertexInputState   = &vertex_input;
    pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState      = &viewport;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState   = &multisample;
    pipeline.pDepthStencilState  = &depth;
    pipeline.pColorBlendState    = &blend;
    pipeline.pDynamicState       = &dynamic;
    pipeline.layout              = pipeline_layout_;
    pipeline.renderPass =
        shadow ? shadow_map_.render_pass() : (wire ? targets_.wire_pass() : targets_.render_pass());
    VkPipeline result = VK_NULL_HANDLE;
    check_vk(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr, &result),
             "scene pipeline");
    return result;
}

void SceneRenderer::releaseSwapChainResources() {
    if (device_ != VK_NULL_HANDLE) {
        for (const auto pipeline : surface_pipelines_) {
            vkDestroyPipeline(device_, pipeline, nullptr);
        }
        for (const auto pipeline : shadow_pipelines_) {
            vkDestroyPipeline(device_, pipeline, nullptr);
        }
        vkDestroyPipeline(device_, wire_pipeline_, nullptr);
        vkDestroyPipeline(device_, background_pipeline_, nullptr);
    }
    surface_pipelines_.fill(VK_NULL_HANDLE);
    shadow_pipelines_.fill(VK_NULL_HANDLE);
    wire_pipeline_       = VK_NULL_HANDLE;
    background_pipeline_ = VK_NULL_HANDLE;
    post_process_.release();
    gizmo_.release();
    ssao_.release();
    targets_.release();
}

void SceneRenderer::releaseResources() {
    releaseSwapChainResources();
    meshes_.clear();
    instances_.release();
    instance_capacity_ = 0;
    uniforms_.release();
    shadow_map_.release();
    ibl_.release();
    reference_texture_.release();
    uploaded_reference_.reset();
    rectangles_.release();
    prt_buffer_.release();
    uploaded_prt_.reset();
    area_shadows_.release();
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    textures_.clear();
    white_texture_.reset();
    draws_.clear();
    vkDestroyDescriptorSetLayout(device_, descriptor_layout_, nullptr);
    pipeline_layout_   = VK_NULL_HANDLE;
    descriptor_pool_   = VK_NULL_HANDLE;
    descriptor_layout_ = VK_NULL_HANDLE;
    scene_revision_    = std::numeric_limits<std::uint64_t>::max();
    device_            = VK_NULL_HANDLE;
}
