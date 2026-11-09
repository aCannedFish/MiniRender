#include "scene_renderer.hpp"
#include "gpu_shader.hpp"
#include <minirender/scene/shadow.hpp>

#include <QElapsedTimer>

#include <array>
#include <cmath>
#include <set>
#include <stdexcept>
#include <vector>

namespace {

using Vertex = minirender::RasterVertex;

// 镜像改变屏幕绕序，切换 frontFace 后仍以原网格外侧作为正面。
std::size_t side_pipeline(const minirender::RenderInstance& instance) {
    if (instance.material.double_sided) {
        return 0;
    }
    return glm::determinant(glm::mat3(instance.object_to_world)) < 0 ? 2 : 1;
}
// std430 按 16 字节对齐。normal 使用 mat4，避免 CPU mat3 的紧密布局与 GLSL 不一致。
struct alignas(16) GpuInstance {
    glm::mat4 object_to_world;
    glm::mat4 normal_to_world;
    glm::vec4 base_color;
    glm::vec4 emission;
    glm::vec4 surface; // roughness, metallic, texture filter, double sided
};
static_assert(sizeof(GpuInstance) == 176);
static_assert(offsetof(GpuInstance, normal_to_world) == 64);
static_assert(offsetof(GpuInstance, base_color) == 128);
static_assert(offsetof(GpuInstance, surface) == 160);
struct DrawIndex {
    std::int32_t light;
    std::int32_t instance;
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
    glm::vec4 viewport;
    glm::mat4 view;
    glm::mat4 projection;
    glm::mat4 inverse_projection;
    glm::vec4 ao_settings; // enabled, radius/m, bias/m, strength
    glm::vec4 ao_filter;   // samples, depth sigma/m, normal exponent, use filtered
    std::array<glm::vec4, minirender::ssao_kernel_size> ao_kernel;
    glm::vec4 ibl; // BRDF ready, maps ready, strength, rotation/radians
};
static_assert(offsetof(FrameUniform, lights) == 96);
static_assert(offsetof(FrameUniform, shadow_view_projection) == 384);
static_assert(offsetof(FrameUniform, view) == 928);
static_assert(offsetof(FrameUniform, ao_kernel) == 1152);
static_assert(offsetof(FrameUniform, ibl) == 2176);
static_assert(sizeof(FrameUniform) == 2192);

} // namespace

SceneRenderer::SceneRenderer(VulkanViewport* window) : window_(window) {}

void SceneRenderer::initResources() {
    device_ = window_->device();
    uniforms_.create(device_, window_->physicalDevice(), sizeof(FrameUniform),
                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    shadow_map_.create(*window_);
    ibl_.create(*window_);
    std::array<VkDescriptorSetLayoutBinding, 8> bindings{};
    auto& binding = bindings[0];
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                   nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                   nullptr};
    bindings[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    for (std::uint32_t i = 4; i < bindings.size(); ++i) {
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    }
    layout.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layout.pBindings = bindings.data();
    check_vk(vkCreateDescriptorSetLayout(device_, &layout, nullptr, &descriptor_layout_),
             "scene descriptor layout");
    minirender::Image white(1, 1);
    white.at(0, 0) = glm::vec3(1);
    white_texture_ = std::make_shared<const minirender::Texture>(std::move(white));
    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(DrawIndex)};
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
    targets_.create(*window_);
    for (std::size_t side = 0; side < surface_pipelines_.size(); ++side) {
        const auto cull = side == 0 ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        const auto front = side == 2 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        surface_pipelines_[side] = create_pipeline(Pass::surface, cull, front);
        shadow_pipelines_[side] = create_pipeline(Pass::shadow, cull, front);
    }
    wire_pipeline_ = create_pipeline(Pass::wire);
    background_pipeline_ = create_pipeline(Pass::background);
    ssao_.create(*window_, targets_, uniforms_.handle(), sizeof(FrameUniform));
    post_process_.create(*window_, targets_, uniforms_.handle(), sizeof(FrameUniform),
                         ssao_.hdr_view(), ssao_.raw_view(), ssao_.filtered_view());
    gizmo_.create(*window_);
    window_->report_swapchain();
}

VkPipeline SceneRenderer::create_pipeline(Pass pass, VkCullModeFlags cull, VkFrontFace front) {
    const bool shadow = pass == Pass::shadow;
    const bool wire = pass == Pass::wire;
    const bool background = pass == Pass::background;
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
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex.handle();
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment ? fragment->handle() : VK_NULL_HANDLE;
    const VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    std::array<VkVertexInputAttributeDescription, 3> attributes{
        {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
         {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
         {5, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)}}};
    if (wire) {
        attributes[1].offset = offsetof(Vertex, geometric_normal);
    }
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = background ? 0 : 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount =
        background ? 0 : (shadow ? 1 : (wire ? 2 : static_cast<std::uint32_t>(attributes.size())));
    vertex_input.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology =
        wire ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = cull;
    raster.frontFace = front;
    raster.lineWidth = 1;
    raster.depthBiasEnable = shadow;
    raster.depthBiasConstantFactor = 1.25F;
    raster.depthBiasSlopeFactor = 1.75F;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = !background;
    depth.depthWriteEnable = !wire && !background;
    depth.depthCompareOp = wire ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_LESS;
    std::array<VkPipelineColorBlendAttachmentState, 3> attachments{};
    for (auto& attachment : attachments) {
        attachment.colorWriteMask = 0xF;
    }
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount =
        shadow ? 0 : (wire ? 1 : static_cast<std::uint32_t>(attachments.size()));
    blend.pAttachments = attachments.data();
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
    pipeline.renderPass =
        shadow ? shadow_map_.render_pass() : (wire ? targets_.wire_pass() : targets_.render_pass());
    VkPipeline result = VK_NULL_HANDLE;
    check_vk(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr, &result),
             "scene pipeline");
    return result;
}

void SceneRenderer::upload_scene() {
    QElapsedTimer timer;
    timer.start();
    const auto& scene = *window_->scene();
    std::set<MeshHandle, std::owner_less<MeshHandle>> used;
    std::vector<Draw> draws;
    std::vector<GpuInstance> instances;
    glm::vec3 minimum(std::numeric_limits<float>::infinity());
    glm::vec3 maximum(-std::numeric_limits<float>::infinity());
    for (const auto& instance : scene.instances) {
        used.insert(instance.mesh);
        if (!meshes_.contains(instance.mesh)) {
            Mesh mesh;
            auto prepared = window_->raster_mesh(instance.mesh);
            if (!prepared) {
                // 同步诊断入口现场准备；交互导入与几何任务在后台完成准备。
                prepared = minirender::prepare_raster_mesh(*instance.mesh);
                window_->supply_raster_mesh(instance.mesh, prepared);
            }
            const auto count = prepared->vertices.size();
            mesh.count = static_cast<std::uint32_t>(count);
            mesh.minimum = prepared->minimum;
            mesh.maximum = prepared->maximum;
            mesh.vertices.create(device_, window_->physicalDevice(), count * sizeof(Vertex),
                                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
            mesh.vertices.write(prepared->vertices.data(), count * sizeof(Vertex));
            mesh.edges.create(device_, window_->physicalDevice(),
                              prepared->edges.size() * sizeof(std::uint32_t),
                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
            mesh.edges.write(prepared->edges.data(),
                             prepared->edges.size() * sizeof(std::uint32_t));
            meshes_.emplace(instance.mesh, std::move(mesh));
            window_->report_mesh_upload();
        }
        const auto& mesh = meshes_.at(instance.mesh);
        // 变换局部 AABB 的八个角点，得到保守世界包围盒，代价不随网格面数增长。
        for (unsigned corner = 0; corner < 8; ++corner) {
            const glm::vec3 point{(corner & 1U) ? mesh.maximum.x : mesh.minimum.x,
                                  (corner & 2U) ? mesh.maximum.y : mesh.minimum.y,
                                  (corner & 4U) ? mesh.maximum.z : mesh.minimum.z};
            const auto world = glm::vec3(instance.object_to_world * glm::vec4(point, 1));
            minimum = glm::min(minimum, world);
            maximum = glm::max(maximum, world);
        }
        const auto texture =
            instance.base_color_texture ? instance.base_color_texture : white_texture_;
        draws.push_back({instance.mesh, texture});
        const auto& material = instance.material;
        const float metallic = material.kind == minirender::MaterialKind::conductor ||
                                       material.kind == minirender::MaterialKind::mirror
                                   ? 1.0F
                                   : 0.0F;
        instances.push_back(
            {instance.object_to_world,
             glm::mat4(instance.normal_to_world),
             glm::vec4(material.base_color, 1),
             glm::vec4(material.emission, 0),
             {material.roughness, metallic, static_cast<float>(material.texture_filter),
              material.double_sided ? 1.0F : 0.0F}});
    }
    // 一帧在途，当前帧开始前 fence 已完成，未引用的网格可以安全回收。
    std::erase_if(meshes_, [&](const auto& entry) { return !used.contains(entry.first); });
    bool descriptors_changed = draws.size() != draws_.size();
    for (std::size_t index = 0; index < std::min(draws.size(), draws_.size()); ++index) {
        descriptors_changed = descriptors_changed || draws[index].texture != draws_[index].texture;
        draws[index].descriptor = draws_[index].descriptor;
    }
    draws_ = std::move(draws);
    const auto required = std::max<std::size_t>(instances.size(), 1);
    if (required > instance_capacity_) {
        instance_capacity_ = required;
        instances_.create(device_, window_->physicalDevice(), required * sizeof(GpuInstance),
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        descriptors_changed = true;
    }
    if (!instances.empty()) {
        instances_.write(instances.data(), instances.size() * sizeof(GpuInstance));
    }
    for (std::size_t index = 0; index < scene.directional_lights.size(); ++index) {
        shadow_matrices_[index] =
            scene.instances.empty()
                ? glm::mat4(1)
                : minirender::directional_shadow_matrix(minimum, maximum,
                                                        scene.directional_lights[index].direction);
    }
    if (descriptors_changed || descriptor_pool_ == VK_NULL_HANDLE) {
        rebuild_texture_descriptors();
    }
    scene_revision_ = window_->scene_revision();
    window_->report_scene_update(static_cast<double>(timer.nsecsElapsed()) / 1e6);
}

void SceneRenderer::rebuild_texture_descriptors() {
    // 当前帧开始前已等 fence，旧 descriptor 和不再使用的纹理可以安全回收。
    vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    descriptor_pool_ = VK_NULL_HANDLE;
    std::set<TextureHandle, std::owner_less<TextureHandle>> used{white_texture_};
    for (const auto& draw : draws_) {
        used.insert(draw.texture);
    }
    std::erase_if(textures_, [&](const auto& entry) { return !used.contains(entry.first); });
    for (const auto& handle : used) {
        if (!textures_.contains(handle)) {
            auto texture = std::make_unique<GpuTexture>();
            texture->create(*window_, *handle);
            textures_.emplace(handle, std::move(texture));
        }
    }
    const auto count = static_cast<std::uint32_t>(draws_.size() + 1);
    const std::array<VkDescriptorPoolSize, 3> sizes{
        {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, count},
         {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6 * count},
         {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, count}}};
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = count;
    pool.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    pool.pPoolSizes = sizes.data();
    check_vk(vkCreateDescriptorPool(device_, &pool, nullptr, &descriptor_pool_),
             "create material descriptor pool");
    const std::vector<VkDescriptorSetLayout> layouts(count, descriptor_layout_);
    std::vector<VkDescriptorSet> sets(count);
    VkDescriptorSetAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocation.descriptorPool = descriptor_pool_;
    allocation.descriptorSetCount = count;
    allocation.pSetLayouts = layouts.data();
    check_vk(vkAllocateDescriptorSets(device_, &allocation, sets.data()),
             "allocate material descriptors");
    for (std::size_t index = 0; index < sets.size(); ++index) {
        const auto& handle = index == 0 ? white_texture_ : draws_[index - 1].texture;
        VkDescriptorBufferInfo uniform{uniforms_.handle(), 0, sizeof(FrameUniform)};
        const std::array<VkDescriptorImageInfo, 2> images{shadow_map_.descriptor(),
                                                          textures_.at(handle)->descriptor()};
        VkDescriptorBufferInfo instances{instances_.handle(), 0,
                                         instance_capacity_ * sizeof(GpuInstance)};
        const auto ibl_images = ibl_.descriptors();
        std::array<VkWriteDescriptorSet, 8> writes{};
        for (std::uint32_t binding = 0; binding < writes.size(); ++binding) {
            writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[binding].dstSet = sets[index];
            writes[binding].dstBinding = binding;
            writes[binding].descriptorCount = 1;
            writes[binding].descriptorType =
                binding == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                             : (binding == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                             : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            if (binding == 0) {
                writes[binding].pBufferInfo = &uniform;
            } else if (binding == 3) {
                writes[binding].pBufferInfo = &instances;
            } else {
                writes[binding].pImageInfo =
                    binding < 3 ? &images[binding - 1] : &ibl_images[binding - 4];
            }
        }
        vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0,
                               nullptr);
    }
    window_->report_descriptor_rebuild();
    descriptor_ = sets[0];
    for (std::size_t index = 0; index < draws_.size(); ++index) {
        draws_[index].descriptor = sets[index + 1];
    }
}

void SceneRenderer::update_ibl_descriptors() {
    const auto images = ibl_.descriptors();
    std::vector<VkDescriptorSet> sets{descriptor_};
    for (const auto& draw : draws_) {
        sets.push_back(draw.descriptor);
    }
    std::vector<VkWriteDescriptorSet> writes;
    for (const auto set : sets) {
        for (std::uint32_t i = 0; i < images.size(); ++i) {
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = set;
            write.dstBinding = 4 + i;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &images[i];
            writes.push_back(write);
        }
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0,
                           nullptr);
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
    frame.counts.y = window_->environment_background() ? 1 : 0;
    frame.display = {window_->exposure(), static_cast<float>(window_->view_mode()),
                     window_->fxaa_enabled() ? 1.0F : 0.0F, 0};
    frame.shadow_view_projection = shadow_matrices_;
    frame.shadow_settings = {window_->shadows_enabled() ? 1.0F : 0.0F, 1.0F / ShadowMap::resolution,
                             0, 0};
    frame.viewport = {1.0F / static_cast<float>(size.width()),
                      1.0F / static_cast<float>(size.height()), camera.near_plane,
                      camera.far_plane};
    frame.view = camera.view_matrix();
    frame.projection = camera.projection_matrix(static_cast<float>(size.width()) /
                                                static_cast<float>(size.height()));
    frame.inverse_projection = glm::inverse(frame.projection);
    const auto& ao = window_->ssao_settings();
    frame.ao_settings = {ao.enabled ? 1.0F : 0.0F, ao.radius, ao.bias, ao.strength};
    frame.ao_filter = {static_cast<float>(ao.samples), std::max(ao.radius * 0.25F, 1e-6F), 16,
                       ao.filtered ? 1.0F : 0.0F};
    static const auto kernel = minirender::make_ssao_kernel();
    frame.ao_kernel = kernel;
    frame.ibl = {
        window_->ibl_lut() ? 1.0F : 0.0F, window_->ibl_maps() ? 1.0F : 0.0F,
        window_->scene()->environment_strength,
        glm::radians(std::remainder(window_->scene()->environment_rotation_degrees, 360.0F))};
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
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1,
                                &descriptor_, 0, nullptr);
        for (std::size_t index = 0; index < draws_.size(); ++index) {
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              shadow_pipelines_[side_pipeline(window_->scene()->instances[index])]);
            const DrawIndex push{light, static_cast<std::int32_t>(index)};
            vkCmdPushConstants(command, pipeline_layout_,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(push), &push);
            const auto& mesh = meshes_.at(draws_[index].mesh);
            const VkDeviceSize offset = 0;
            const auto buffer = mesh.vertices.handle();
            vkCmdBindVertexBuffers(command, 0, 1, &buffer, &offset);
            vkCmdDraw(command, mesh.count, 1, 0, 0);
        }
        vkCmdEndRenderPass(command);
    }
}

void SceneRenderer::render_wire(VkCommandBuffer command) {
    // 后处理始终绑定 wire 图像，首次先清除使其布局有效；以后仅在线框模式绘制。
    if (window_->view_mode() != 3 && targets_.wire_initialized()) {
        return;
    }
    targets_.begin_wire(command);
    if (window_->view_mode() == 3) {
        const auto size = window_->swapChainImageSize();
        const VkViewport viewport{
            0, 0, static_cast<float>(size.width()), static_cast<float>(size.height()), 0, 1};
        const VkRect2D scissor{
            {0, 0},
            {static_cast<std::uint32_t>(size.width()), static_cast<std::uint32_t>(size.height())}};
        vkCmdSetViewport(command, 0, 1, &viewport);
        vkCmdSetScissor(command, 0, 1, &scissor);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, wire_pipeline_);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1,
                                &descriptor_, 0, nullptr);
        for (std::size_t index = 0; index < draws_.size(); ++index) {
            const auto& mesh = meshes_.at(draws_[index].mesh);
            const auto buffer = mesh.vertices.handle();
            const VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(command, 0, 1, &buffer, &offset);
            vkCmdBindIndexBuffer(command, mesh.edges.handle(), 0, VK_INDEX_TYPE_UINT32);
            const DrawIndex push{0, static_cast<std::int32_t>(index)};
            vkCmdPushConstants(command, pipeline_layout_,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(push), &push);
            vkCmdDrawIndexed(command, mesh.count * 2, 1, 0, 0, 0);
        }
    }
    vkCmdEndRenderPass(command);
}

void SceneRenderer::startNextFrame() {
    const bool ibl_changed = ibl_.update(*window_, window_->ibl_maps(), window_->ibl_lut());
    if (scene_revision_ != window_->scene_revision()) {
        upload_scene();
    }
    if (ibl_changed) {
        update_ibl_descriptors();
        window_->report_ibl_update();
    }
    ibl_.record_upload(window_->currentCommandBuffer());
    for (auto& [handle, texture] : textures_) {
        texture->retire_upload();
        texture->record_upload(window_->currentCommandBuffer());
    }
    update_uniforms();
    render_shadow_maps(window_->currentCommandBuffer());
    const auto size = window_->swapChainImageSize();
    const VkExtent2D extent{static_cast<std::uint32_t>(size.width()),
                            static_cast<std::uint32_t>(size.height())};
    const auto command = window_->currentCommandBuffer();
    targets_.begin(command);
    const VkViewport viewport{
        0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height), 0, 1};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    if (window_->ibl_maps() && window_->environment_background()) {
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, background_pipeline_);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1,
                                &descriptor_, 0, nullptr);
        vkCmdDraw(command, 3, 1, 0, 0);
    }
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1,
                            &descriptor_, 0, nullptr);
    for (std::size_t index = 0; index < draws_.size(); ++index) {
        const auto& draw = draws_[index];
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          surface_pipelines_[side_pipeline(window_->scene()->instances[index])]);
        const auto& mesh = meshes_.at(draw.mesh);
        const VkDeviceSize offset = 0;
        const auto buffer = mesh.vertices.handle();
        vkCmdBindVertexBuffers(command, 0, 1, &buffer, &offset);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1,
                                &draw.descriptor, 0, nullptr);
        const DrawIndex push{0, static_cast<std::int32_t>(index)};
        vkCmdPushConstants(command, pipeline_layout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        vkCmdDraw(command, mesh.count, 1, 0, 0);
    }
    vkCmdEndRenderPass(command);
    render_wire(command);
    ssao_.draw(*window_);
    if (capture_hdr_) {
        targets_.record_readback(command, ssao_.hdr_image());
        capture_hdr_ = false;
    }
    post_process_.draw(*window_, [this] {
        gizmo_.draw(*window_, window_->gizmo_layout(), window_->highlighted_gizmo_handle());
    });
    window_->report_frame();
    window_->frameReady();
    window_->requestUpdate();
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
    wire_pipeline_ = VK_NULL_HANDLE;
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
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    textures_.clear();
    white_texture_.reset();
    draws_.clear();
    vkDestroyDescriptorSetLayout(device_, descriptor_layout_, nullptr);
    pipeline_layout_ = VK_NULL_HANDLE;
    descriptor_pool_ = VK_NULL_HANDLE;
    descriptor_layout_ = VK_NULL_HANDLE;
    scene_revision_ = std::numeric_limits<std::uint64_t>::max();
    device_ = VK_NULL_HANDLE;
}

void SceneRenderer::request_hdr_capture() {
    capture_hdr_ = true;
}
minirender::Image SceneRenderer::read_hdr_capture() {
    return targets_.readback();
}
