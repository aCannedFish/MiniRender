#include "scene_renderer.hpp"
#include <minirender/scene/shadow.hpp>

#include <QFile>

#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {

struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec3 color;
    glm::vec3 emission;
    glm::vec2 material; // roughness, metallic
};
struct alignas(16) GpuLight {
    glm::vec4 direction;
    glm::vec4 radiance;
};
struct alignas(16) FrameUniform {
    glm::mat4 view_projection;
    glm::vec4 camera_position;
    glm::vec4 environment;
    std::array<GpuLight, 8> lights;
    glm::ivec4 counts;
    glm::vec4 display;
    std::array<glm::mat4, 8> shadow_view_projection;
    glm::vec4 shadow_settings;
};
static_assert(offsetof(FrameUniform, lights) == 96);
static_assert(offsetof(FrameUniform, shadow_view_projection) == 384);
static_assert(sizeof(FrameUniform) == 912);

// shader module 只需存活到 pipeline 创建完成；异常路径同样释放。
class Shader {
  public:
    Shader(VkDevice device, const char* resource) : device_(device) {
        QFile file(QString::fromUtf8(resource));
        if (!file.open(QIODevice::ReadOnly)) {
            throw std::runtime_error("Cannot open scene SPIR-V");
        }
        const auto bytes = file.readAll();
        if (bytes.isEmpty() || bytes.size() % 4 != 0) {
            throw std::runtime_error("Invalid SPIR-V size");
        }
        std::vector<std::uint32_t> words(static_cast<std::size_t>(bytes.size()) / 4);
        std::memcpy(words.data(), bytes.constData(), static_cast<std::size_t>(bytes.size()));
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = static_cast<std::size_t>(bytes.size());
        info.pCode = words.data();
        check_vk(vkCreateShaderModule(device_, &info, nullptr, &handle_), "create scene shader");
    }
    ~Shader() {
        vkDestroyShaderModule(device_, handle_, nullptr);
    }
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
    VkShaderModule handle() const {
        return handle_;
    }

  private:
    VkDevice device_;
    VkShaderModule handle_ = VK_NULL_HANDLE;
};

} // namespace

SceneRenderer::SceneRenderer(VulkanViewport* window) : window_(window) {}

void SceneRenderer::initResources() {
    device_ = window_->device();
    uniforms_.create(device_, window_->physicalDevice(), sizeof(FrameUniform),
                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    shadow_map_.create(*window_);
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    auto& binding = bindings[0];
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                   nullptr};
    layout.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layout.pBindings = bindings.data();
    check_vk(vkCreateDescriptorSetLayout(device_, &layout, nullptr, &descriptor_layout_),
             "scene descriptor layout");
    const std::array<VkDescriptorPoolSize, 2> sizes{
        {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}}};
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = 1;
    pool.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    pool.pPoolSizes = sizes.data();
    check_vk(vkCreateDescriptorPool(device_, &pool, nullptr, &descriptor_pool_),
             "scene descriptor pool");
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = descriptor_pool_;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &descriptor_layout_;
    check_vk(vkAllocateDescriptorSets(device_, &allocate, &descriptor_), "scene descriptor");
    VkDescriptorBufferInfo buffer{uniforms_.handle(), 0, sizeof(FrameUniform)};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptor_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &buffer;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    const auto shadow_descriptor = shadow_map_.descriptor();
    write.dstBinding = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pBufferInfo = nullptr;
    write.pImageInfo = &shadow_descriptor;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(std::int32_t)};
    VkPipelineLayoutCreateInfo pipeline_layout{};
    pipeline_layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout.pushConstantRangeCount = 1;
    pipeline_layout.pPushConstantRanges = &push;
    pipeline_layout.setLayoutCount = 1;
    pipeline_layout.pSetLayouts = &descriptor_layout_;
    check_vk(vkCreatePipelineLayout(device_, &pipeline_layout, nullptr, &pipeline_layout_),
             "scene pipeline layout");
}

void SceneRenderer::initSwapChainResources() {
    pipeline_ = create_pipeline(false);
    shadow_pipeline_ = create_pipeline(true);
    window_->report_swapchain();
}

VkPipeline SceneRenderer::create_pipeline(bool shadow) {
    const Shader vertex(device_, shadow ? ":/shaders/shadow.vert.spv" : ":/shaders/scene.vert.spv");
    std::unique_ptr<Shader> fragment;
    if (!shadow) {
        fragment = std::make_unique<Shader>(device_, ":/shaders/scene.frag.spv");
    }
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    for (auto& stage : stages) {
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.pName = "main";
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex.handle();
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment ? fragment->handle() : VK_NULL_HANDLE;
    const VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const std::array<VkVertexInputAttributeDescription, 5> attributes{
        {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
         {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
         {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, color)},
         {3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, emission)},
         {4, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, material)}}};
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount =
        shadow ? 1 : static_cast<std::uint32_t>(attributes.size());
    vertex_input.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1;
    raster.depthBiasEnable = shadow;
    raster.depthBiasConstantFactor = 1.25F;
    raster.depthBiasSlopeFactor = 1.75F;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = shadow ? 0 : 1;
    blend.pAttachments = &attachment;
    const std::array<VkDynamicState, 2> states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(states.size());
    dynamic.pDynamicStates = states.data();
    VkGraphicsPipelineCreateInfo pipeline{};
    pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline.stageCount = shadow ? 1 : static_cast<std::uint32_t>(stages.size());
    pipeline.pStages = stages.data();
    pipeline.pVertexInputState = &vertex_input;
    pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &viewport;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &multisample;
    pipeline.pDepthStencilState = &depth;
    pipeline.pColorBlendState = &blend;
    pipeline.pDynamicState = &dynamic;
    pipeline.layout = pipeline_layout_;
    pipeline.renderPass = shadow ? shadow_map_.render_pass() : window_->defaultRenderPass();
    VkPipeline result = VK_NULL_HANDLE;
    check_vk(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr, &result),
             "scene pipeline");
    return result;
}

void SceneRenderer::upload_scene() {
    std::vector<Vertex> vertices;
    for (const auto& instance : window_->scene()->instances) {
        const auto& material = instance.material;
        const float metallic = material.kind == minirender::MaterialKind::conductor ||
                                       material.kind == minirender::MaterialKind::mirror
                                   ? 1.0F
                                   : 0.0F;
        for (const auto& triangle : instance.mesh->triangles) {
            for (const auto& corner : triangle.corners) {
                vertices.push_back(
                    {glm::vec3(instance.object_to_world *
                               glm::vec4(instance.mesh->positions[corner.position], 1)),
                     glm::normalize(instance.normal_to_world * corner.normal),
                     material.base_color,
                     material.emission,
                     {material.roughness, metallic}});
            }
        }
    }
    if (vertices.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("Scene vertex count exceeds draw limit");
    }
    vertex_count_ = static_cast<std::uint32_t>(vertices.size());
    vertices_.release();
    if (!vertices.empty()) {
        vertices_.create(device_, window_->physicalDevice(), vertices.size() * sizeof(Vertex),
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        vertices_.write(vertices.data(), vertices.size() * sizeof(Vertex));
    }
    for (std::size_t index = 0; index < window_->scene()->directional_lights.size(); ++index) {
        shadow_matrices_[index] = minirender::directional_shadow_matrix(
            *window_->scene(), window_->scene()->directional_lights[index].direction);
    }
    scene_revision_ = window_->scene_revision();
}

void SceneRenderer::update_uniforms() {
    const auto size = window_->swapChainImageSize();
    const auto& camera = window_->camera();
    FrameUniform frame{};
    frame.view_projection = camera.projection_matrix(static_cast<float>(size.width()) /
                                                     static_cast<float>(size.height())) *
                            camera.view_matrix();
    frame.camera_position = glm::vec4(camera.position, 1);
    frame.environment = glm::vec4(window_->scene()->environment, 0);
    const auto& lights = window_->scene()->directional_lights;
    if (lights.size() > frame.lights.size()) {
        throw std::runtime_error("Realtime preview supports at most 8 directional lights");
    }
    for (std::size_t i = 0; i < lights.size(); ++i) {
        frame.lights[i].direction = glm::vec4(lights[i].direction, 0);
        frame.lights[i].radiance = glm::vec4(lights[i].color * lights[i].intensity, 0);
    }
    frame.counts.x = static_cast<int>(lights.size());
    frame.display = {window_->exposure(), static_cast<float>(window_->view_mode()), 0, 0};
    frame.shadow_view_projection = shadow_matrices_;
    frame.shadow_settings = {window_->shadows_enabled() ? 1.0F : 0.0F, 1.0F / ShadowMap::resolution,
                             0, 0};
    uniforms_.write(&frame, sizeof(frame));
}

void SceneRenderer::render_shadow_maps(VkCommandBuffer command) {
    shadow_map_.prepare(command);
    if (!window_->shadows_enabled()) {
        return;
    }
    const VkViewport viewport{
        0, 0, static_cast<float>(ShadowMap::resolution), static_cast<float>(ShadowMap::resolution),
        0, 1};
    const VkRect2D scissor{{0, 0}, {ShadowMap::resolution, ShadowMap::resolution}};
    for (std::int32_t light = 0;
         light < static_cast<std::int32_t>(window_->scene()->directional_lights.size()); ++light) {
        shadow_map_.begin(command, static_cast<std::uint32_t>(light));
        vkCmdSetViewport(command, 0, 1, &viewport);
        vkCmdSetScissor(command, 0, 1, &scissor);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_pipeline_);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1,
                                &descriptor_, 0, nullptr);
        vkCmdPushConstants(command, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(light),
                           &light);
        if (vertex_count_ != 0) {
            const VkDeviceSize offset = 0;
            const auto buffer = vertices_.handle();
            vkCmdBindVertexBuffers(command, 0, 1, &buffer, &offset);
            vkCmdDraw(command, vertex_count_, 1, 0, 0);
        }
        vkCmdEndRenderPass(command);
    }
}

void SceneRenderer::startNextFrame() {
    if (scene_revision_ != window_->scene_revision()) {
        upload_scene();
    }
    update_uniforms();
    render_shadow_maps(window_->currentCommandBuffer());
    const auto size = window_->swapChainImageSize();
    const VkExtent2D extent{static_cast<std::uint32_t>(size.width()),
                            static_cast<std::uint32_t>(size.height())};
    std::array<VkClearValue, 2> clear{};
    clear[0].color = {{0.012F, 0.018F, 0.028F, 1.0F}};
    clear[1].depthStencil = {1, 0};
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = window_->defaultRenderPass();
    begin.framebuffer = window_->currentFramebuffer();
    begin.renderArea.extent = extent;
    begin.clearValueCount = static_cast<std::uint32_t>(clear.size());
    begin.pClearValues = clear.data();
    const auto command = window_->currentCommandBuffer();
    vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{
        0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height), 0, 1};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1,
                            &descriptor_, 0, nullptr);
    if (vertex_count_ != 0) {
        const VkDeviceSize offset = 0;
        const auto buffer = vertices_.handle();
        vkCmdBindVertexBuffers(command, 0, 1, &buffer, &offset);
        vkCmdDraw(command, vertex_count_, 1, 0, 0);
    }
    vkCmdEndRenderPass(command);
    window_->report_frame();
    window_->frameReady();
    window_->requestUpdate();
}

void SceneRenderer::releaseSwapChainResources() {
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        vkDestroyPipeline(device_, shadow_pipeline_, nullptr);
    }
    pipeline_ = VK_NULL_HANDLE;
    shadow_pipeline_ = VK_NULL_HANDLE;
}

void SceneRenderer::releaseResources() {
    releaseSwapChainResources();
    vertices_.release();
    uniforms_.release();
    shadow_map_.release();
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    vkDestroyDescriptorSetLayout(device_, descriptor_layout_, nullptr);
    pipeline_layout_ = VK_NULL_HANDLE;
    descriptor_pool_ = VK_NULL_HANDLE;
    descriptor_layout_ = VK_NULL_HANDLE;
    scene_revision_ = std::numeric_limits<std::uint64_t>::max();
    device_ = VK_NULL_HANDLE;
}
