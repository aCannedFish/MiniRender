// 三角网格实时渲染、资源缓存、阴影与显示管线连接。
// 本模块接口见 scene_renderer.hpp。

#include "scene_renderer.hpp"
#include "gpu_cloth.hpp"
#include "gpu_shader.hpp"
#include <minirender/scene/shadow.hpp>

#include <QElapsedTimer>

#include <array>
#include <bit>
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
    glm::mat4  object_to_world;
    glm::mat4  normal_to_world;
    glm::vec4  base_color;
    glm::vec4  emission;
    glm::vec4  surface; // roughness, metallic, texture filter, double sided
    GpuCloth   cloth;
    glm::uvec4 geometry;
    glm::mat4  previous_model;
};

static_assert(sizeof(GpuInstance) == 880);
static_assert(offsetof(GpuInstance, normal_to_world) == 64);
static_assert(offsetof(GpuInstance, base_color) == 128);
static_assert(offsetof(GpuInstance, surface) == 160);
static_assert(offsetof(GpuInstance, cloth) == 176);

struct DrawIndex {
    std::int32_t light;
    std::int32_t instance;
};

struct alignas(16) GpuLight {
    glm::vec4 direction;
    glm::vec4 radiance;
};

struct alignas(16) FrameUniform {
    glm::mat4                view_projection;
    glm::vec4                camera_position;
    glm::vec4                environment;
    std::array<GpuLight, 8>  lights;
    glm::ivec4               counts;
    glm::vec4                display;
    std::array<glm::mat4, 8> shadow_view_projection;
    glm::vec4                shadow_settings;
    glm::vec4                viewport;
    glm::mat4                view;
    glm::mat4                projection;
    glm::mat4                inverse_projection;
    glm::vec4                ao_settings; // enabled, radius/m, bias/m, strength
    glm::vec4                ao_filter;   // samples, depth sigma/m, normal exponent, use filtered
    std::array<glm::vec4, minirender::ssao_kernel_size> ao_kernel;
    glm::vec4 ibl;           // BRDF ready, maps ready, strength, rotation/radians
    glm::vec4 preview_edges; // effective strength, physical pixels per logical pixel, reserved
    glm::vec4 teaching_light, teaching_surface;
    glm::vec4 area_settings;
    std::array<glm::vec4, 9> prt_light;
    glm::vec4                screen_trace, screen_options;
    glm::mat4                previous_vp;
    glm::vec4                history_settings;
};

static_assert(offsetof(FrameUniform, lights) == 96);
static_assert(offsetof(FrameUniform, shadow_view_projection) == 384);
static_assert(offsetof(FrameUniform, view) == 928);
static_assert(offsetof(FrameUniform, ao_kernel) == 1152);
static_assert(offsetof(FrameUniform, ibl) == 2176);
static_assert(offsetof(FrameUniform, preview_edges) == 2192);
static_assert(sizeof(FrameUniform) == 2512);

} // namespace

SceneRenderer::SceneRenderer(VulkanViewport* window) : window_(window) {}

void SceneRenderer::initResources() {
    device_ = window_->device();
    uniforms_.create(device_, window_->physicalDevice(), sizeof(FrameUniform),
                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    shadow_map_.create(*window_);
    ibl_.create(*window_);
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
    ssao_.create(*window_, targets_, uniforms_.handle(), sizeof(FrameUniform));
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

void SceneRenderer::upload_scene() {
    shadow_depth_ready_ = false;
    QElapsedTimer timer;
    timer.start();
    const auto& scene = *window_->scene();
    subsurface_previews_.begin_scene();
    std::set<MeshHandle, std::owner_less<MeshHandle>> used;
    std::vector<Draw>                                 draws;
    std::vector<GpuInstance>                          instances;
    glm::vec3 minimum(std::numeric_limits<float>::infinity());
    glm::vec3 maximum(-std::numeric_limits<float>::infinity());
    // 先收集全部可见来源，避免复用仍被另一个实例引用的旧缓冲。
    prior_scene_   = current_scene_;
    current_scene_ = window_->scene();
    rigid_update_ =
        prior_scene_ && prior_scene_->instances.size() == scene.instances.size() &&
        prior_scene_->environment == scene.environment &&
        prior_scene_->environment_texture == scene.environment_texture &&
        prior_scene_->environment_strength == scene.environment_strength &&
        prior_scene_->environment_rotation_degrees == scene.environment_rotation_degrees &&
        prior_scene_->directional_lights.size() == scene.directional_lights.size();
    if (rigid_update_) {
        for (std::size_t i = 0; i < scene.directional_lights.size(); ++i) {
            const auto& a = prior_scene_->directional_lights[i];
            const auto& b = scene.directional_lights[i];
            if (a.direction != b.direction || a.color != b.color || a.intensity != b.intensity) {
                rigid_update_ = false;
            }
        }
    }
    std::uint32_t corner_offset = 0;
    for (const auto& instance : scene.instances) {
        used.insert(instance.mesh);
    }
    for (const auto& instance : scene.instances) {
        if (!meshes_.contains(instance.mesh)) {
            Mesh mesh;
            auto prepared = window_->raster_mesh(instance.mesh);
            if (!prepared) {
                // 同步诊断入口现场准备；交互导入与几何任务在后台完成准备。
                prepared = minirender::prepare_raster_mesh(*instance.mesh);
                window_->supply_raster_mesh(instance.mesh, prepared);
            }
            const auto count = prepared->vertices.size();
            const auto reusable =
                std::find_if(meshes_.begin(), meshes_.end(), [&](const auto& entry) {
                    return !used.contains(entry.first) && entry.second.count == count;
                });
            if (reusable != meshes_.end()) {
                mesh = std::move(reusable->second);
                meshes_.erase(reusable);
                window_->report_vertex_buffer_reuse();
            } else {
                mesh.vertices.create(device_, window_->physicalDevice(), count * sizeof(Vertex),
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
                mesh.edges.create(device_, window_->physicalDevice(),
                                  prepared->edges.size() * sizeof(std::uint32_t),
                                  VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
                mesh.edges.write(prepared->edges.data(),
                                 prepared->edges.size() * sizeof(std::uint32_t));
                window_->report_mesh_allocation();
            }
            mesh.count   = static_cast<std::uint32_t>(count);
            mesh.minimum = prepared->minimum;
            mesh.maximum = prepared->maximum;
            mesh.vertices.write(prepared->vertices.data(), count * sizeof(Vertex));
            meshes_.emplace(instance.mesh, std::move(mesh));
            window_->report_mesh_upload();
        }
        const auto& mesh = meshes_.at(instance.mesh);
        // 变换局部 AABB 的八个角点，得到保守世界包围盒，代价不随网格面数增长。
        for (unsigned corner = 0; corner < 8; ++corner) {
            const glm::vec3 point{(corner & 1U) ? mesh.maximum.x : mesh.minimum.x,
                                  (corner & 2U) ? mesh.maximum.y : mesh.minimum.y,
                                  (corner & 4U) ? mesh.maximum.z : mesh.minimum.z};
            const auto      world = glm::vec3(instance.object_to_world * glm::vec4(point, 1));
            minimum               = glm::min(minimum, world);
            maximum               = glm::max(maximum, world);
        }
        const auto texture =
            instance.base_color_texture ? instance.base_color_texture : white_texture_;
        draws.push_back({instance.mesh, texture,
                         instance.normal_texture ? instance.normal_texture : white_texture_});
        const auto& material   = instance.material;
        const bool  subsurface = material.kind == minirender::MaterialKind::subsurface;
        const auto  color = subsurface ? subsurface_previews_.color(material) : material.base_color;
        const float metallic = material.kind == minirender::MaterialKind::conductor ||
                                       material.kind == minirender::MaterialKind::mirror
                                   ? 1.0F
                                   : 0.0F;
        const auto  stable   = surface_ids_
                                   .try_emplace(instance.object_id,
                                                static_cast<std::uint32_t>(surface_ids_.size() + 1))
                                   .first->second;
        auto        previous_model = instance.object_to_world;
        if (rigid_update_) {
            const auto& prev = prior_scene_->instances[instances.size()];
            rigid_update_    = prev.object_id == instance.object_id && prev.mesh == instance.mesh &&
                               prev.material.base_color == material.base_color &&
                               prev.material.emission == material.emission &&
                               prev.material.kind == material.kind &&
                               prev.material.roughness == material.roughness &&
                               prev.material.compensate_energy == material.compensate_energy &&
                               prev.base_color_texture == instance.base_color_texture &&
                               prev.normal_texture == instance.normal_texture &&
                               prev.material.normal_scale == material.normal_scale &&
                               prev.material.normal_flip_y == material.normal_flip_y &&
                               prev.material.double_sided == material.double_sided &&
                               (glm::length(material.emission) == 0 ||
                                prev.object_to_world == instance.object_to_world);
            previous_model   = prev.object_to_world;
        }
        instances.push_back(
            {instance.object_to_world,
             glm::mat4(instance.normal_to_world),
             glm::vec4(color, 1),
             glm::vec4(material.emission, material.kind == minirender::MaterialKind::conductor &&
                                                  material.roughness > .001F
                                              ? (material.compensate_energy ? 1.F : 0.F)
                                              : -1.F),
             {subsurface ? .35F : material.roughness, metallic,
              static_cast<float>(material.texture_filter), material.double_sided ? 1.0F : 0.0F},
             pack_cloth(material),
             {corner_offset, stable,
              instance.normal_texture
                  ? std::bit_cast<std::uint32_t>(material.normal_scale *
                                                 (material.normal_flip_y ? -1.F : 1.F))
                  : 0U,
              0},
             previous_model});
        corner_offset += static_cast<std::uint32_t>(instance.mesh->triangles.size() * 3);
    }
    subsurface_previews_.end_scene();
    // 一帧在途，当前帧开始前 fence 已完成，未引用的网格可以安全回收。
    std::erase_if(meshes_, [&](const auto& entry) { return !used.contains(entry.first); });
    bool descriptors_changed = draws.size() != draws_.size();
    for (std::size_t index = 0; index < std::min(draws.size(), draws_.size()); ++index) {
        descriptors_changed     = descriptors_changed ||
                                  draws[index].texture != draws_[index].texture ||
                                  draws[index].normal_texture != draws_[index].normal_texture;
        draws[index].descriptor = draws_[index].descriptor;
    }
    draws_              = std::move(draws);
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
    // 接触半径随场景几何尺度变化，滚轮缩放相机不会改变其世界空间范围。
    preview_radius_ = scene.instances.empty()
                          ? 0.2F
                          : std::clamp(glm::length(maximum - minimum) * 0.04F, 1e-5F, 1000.0F);
    for (std::size_t index = 0; index < scene.directional_lights.size(); ++index) {
        shadow_matrices_[index] =
            scene.instances.empty()
                ? glm::mat4(1)
                : minirender::directional_shadow_matrix(minimum, maximum,
                                                        scene.directional_lights[index].direction);
    }
    minimum_                          = minimum;
    maximum_                          = maximum;
    const bool shadow_buffers_changed = area_shadows_.configure(
        *window_, scene, minimum, maximum, area_grid(), window_->scene_revision());
    const bool rectangle_buffer_changed = rectangles_.update(*window_, scene.rectangles);
    const bool prt_changed              = update_prt();
    if (descriptors_changed || prt_changed || shadow_buffers_changed || rectangle_buffer_changed ||
        descriptor_pool_ == VK_NULL_HANDLE) {
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
        used.insert(draw.normal_texture);
    }
    std::erase_if(textures_, [&](const auto& entry) { return !used.contains(entry.first); });
    for (const auto& handle : used) {
        if (!textures_.contains(handle)) {
            auto texture = std::make_unique<GpuTexture>();
            texture->create(*window_, *handle);
            textures_.emplace(handle, std::move(texture));
        }
    }
    const auto                                count = static_cast<std::uint32_t>(draws_.size() + 1);
    const std::array<VkDescriptorPoolSize, 3> sizes{
        {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, count},
         {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 10 * count},
         {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * count}}};
    VkDescriptorPoolCreateInfo pool{};
    pool.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets       = count;
    pool.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    pool.pPoolSizes    = sizes.data();
    check_vk(vkCreateDescriptorPool(device_, &pool, nullptr, &descriptor_pool_),
             "create material descriptor pool");
    const std::vector<VkDescriptorSetLayout> layouts(count, descriptor_layout_);
    std::vector<VkDescriptorSet>             sets(count);
    VkDescriptorSetAllocateInfo              allocation{};
    allocation.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocation.descriptorPool     = descriptor_pool_;
    allocation.descriptorSetCount = count;
    allocation.pSetLayouts        = layouts.data();
    check_vk(vkAllocateDescriptorSets(device_, &allocation, sets.data()),
             "allocate material descriptors");
    for (std::size_t index = 0; index < sets.size(); ++index) {
        const auto&            handle = index == 0 ? white_texture_ : draws_[index - 1].texture;
        VkDescriptorBufferInfo uniform{uniforms_.handle(), 0, sizeof(FrameUniform)};
        const std::array<VkDescriptorImageInfo, 2> images{shadow_map_.descriptor(),
                                                          textures_.at(handle)->descriptor()};
        VkDescriptorBufferInfo               instances{instances_.handle(), 0,
                                                       instance_capacity_ * sizeof(GpuInstance)};
        const auto                           ibl_images = ibl_.descriptors();
        std::array<VkWriteDescriptorSet, 15> writes{};
        const auto                           rectangle_tables = rectangles_.tables();
        const auto                           rectangle_buffer = rectangles_.buffer();
        const auto                           projections      = area_shadows_.projections();
        const auto                           area_depth       = area_shadows_.depth();
        const VkDescriptorBufferInfo         prt{prt_buffer_.handle(), 0, VK_WHOLE_SIZE};
        const auto                           normal_texture =
            textures_.at(index ? draws_[index - 1].normal_texture : white_texture_)->descriptor();
        for (std::uint32_t binding = 0; binding < writes.size(); ++binding) {
            writes[binding].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[binding].dstSet          = sets[index];
            writes[binding].dstBinding      = binding;
            writes[binding].descriptorCount = 1;
            writes[binding].descriptorType =
                binding == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                             : ((binding == 3 || binding == 10 || binding == 11 || binding == 13)
                                    ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                    : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            if (binding == 0) {
                writes[binding].pBufferInfo = &uniform;
            } else if (binding == 14) {
                writes[binding].pImageInfo = &normal_texture;
            } else if (binding == 13) {
                writes[binding].pBufferInfo = &prt;
            } else if (binding == 11) {
                writes[binding].pBufferInfo = &projections;
            } else if (binding == 12) {
                writes[binding].pImageInfo = &area_depth;
            } else if (binding == 10) {
                writes[binding].pBufferInfo = &rectangle_buffer;
            } else if (binding >= 8) {
                writes[binding].pImageInfo = &rectangle_tables[binding - 8];
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
    const auto                   images = ibl_.descriptors();
    std::vector<VkDescriptorSet> sets{descriptor_};
    for (const auto& draw : draws_) {
        sets.push_back(draw.descriptor);
    }
    std::vector<VkWriteDescriptorSet> writes;
    for (const auto set : sets) {
        for (std::uint32_t i = 0; i < images.size(); ++i) {
            VkWriteDescriptorSet write{};
            write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet          = set;
            write.dstBinding      = 4 + i;
            write.descriptorCount = 1;
            write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo      = &images[i];
            writes.push_back(write);
        }
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0,
                           nullptr);
}

void SceneRenderer::update_uniforms(int mode, bool reference) {
    const auto   size   = window_->swapChainImageSize();
    const auto&  camera = window_->camera();
    FrameUniform frame{};
    frame.view_projection = camera.projection_matrix(static_cast<float>(size.width()) /
                                                     static_cast<float>(size.height())) *
                            camera.view_matrix();
    frame.camera_position = glm::vec4(camera.position, 1);
    frame.environment     = glm::vec4(window_->scene()->environment, 0);
    const auto& lights    = window_->scene()->directional_lights;
    if (lights.size() > frame.lights.size()) {
        throw std::runtime_error("Realtime preview supports at most 8 directional lights");
    }
    for (std::size_t i = 0; i < lights.size(); ++i) {
        frame.lights[i].direction = glm::vec4(lights[i].direction, 0);
        frame.lights[i].radiance  = glm::vec4(lights[i].color * lights[i].intensity, 0);
    }
    frame.counts.x      = static_cast<int>(lights.size());
    frame.counts.y      = window_->environment_background() ? 1 : 0;
    frame.display       = {window_->exposure(), static_cast<float>(mode),
                           window_->fxaa_enabled() ? 1.0F : 0.0F, 0};
    frame.preview_edges = {window_->preview_outline() ? window_->preview_outline_strength() : 0.0F,
                           static_cast<float>(window_->devicePixelRatio()), 0, 0};
    frame.shadow_view_projection = shadow_matrices_;
    frame.shadow_settings        = {window_->shadows_enabled() ? 1.F : 0.F,
                                    window_->shadow_filter_radius(),
                                    static_cast<float>(window_->effective_shadow(reference)),
                                    std::tan(glm::radians(window_->light_half_angle()))};
    frame.counts.z               = window_->shadow_debug_light();
    frame.counts.w               = reference ? window_->reference_energy() : -1;
    frame.viewport               = {1.0F / static_cast<float>(size.width()),
                                    1.0F / static_cast<float>(size.height()), camera.near_plane,
                                    camera.far_plane};
    frame.view                   = camera.view_matrix();
    frame.projection             = camera.projection_matrix(static_cast<float>(size.width()) /
                                                            static_cast<float>(size.height()));
    frame.inverse_projection     = glm::inverse(frame.projection);
    const auto& ao               = window_->ssao_settings();
    const bool  preview          = mode == VulkanViewport::base_color_preview_mode;
    const float radius           = preview ? preview_radius_ : ao.radius;
    frame.ao_settings            = {preview ? (window_->preview_occlusion() ? 1.0F : 0.0F)
                                            : (ao.enabled ? 1.0F : 0.0F),
                                    radius, preview ? radius * 0.02F : ao.bias, ao.strength};
    frame.ao_filter = {preview ? 32.0F : static_cast<float>(ao.samples),
                       std::max(radius * 0.25F, 1e-6F), 16, preview || ao.filtered ? 1.0F : 0.0F};
    if (preview) {
        frame.ao_settings.w = 1.0F;
    }
    static const auto kernel = minirender::make_ssao_kernel();
    frame.ao_kernel          = kernel;
    frame.ibl                = {
        window_->ibl_lut() ? 1.0F : 0.0F, window_->ibl_maps() ? 1.0F : 0.0F,
        window_->scene()->environment_strength,
        glm::radians(std::remainder(window_->scene()->environment_rotation_degrees, 360.0F))};
    for (std::size_t k = 0; k < 9; ++k) {
        frame.prt_light[k] =
            glm::vec4(glm::vec3(window_->prt_lighting()[k]), uploaded_prt_ ? 1 : 0);
    }
    frame.screen_trace   = window_->screen_trace();
    frame.screen_options = {window_->screen_hiz() ? 1.F : 0.F, 0, 0, 0};
    auto& history        = histories_[reference ? 1 : 0];
    if (mode == 19 || mode == 20) {
        const auto forward = glm::normalize(camera.target - camera.position);
        const auto previous =
            history.age ? glm::normalize(history.camera.target - history.camera.position) : forward;
        const bool cut =
            history.age && (glm::dot(forward, previous) < .75F ||
                            glm::length(camera.position - history.camera.position) >
                                .5F * std::max(.1F, glm::length(camera.target - camera.position)) ||
                            camera.vertical_fov_degrees != history.camera.vertical_fov_degrees);
        const bool moved   = history.scene != current_scene_;
        const bool valid   = history.age && history.epoch == window_->history_epoch() && !cut &&
                             (!moved || (rigid_update_ && history.scene == prior_scene_)) &&
                             (history.frame + 1 >= window_->rendered_frames() || history.age >= 32);
        const auto options = window_->temporal_settings();
        frame.previous_vp  = history.vp;
        frame.history_settings = {valid ? 1.F : 0.F, valid && moved ? 1.F : 0.F, options.x,
                                  options.y};
        history.age            = valid ? std::min(32U, history.age + 1) : 1;
        frame.screen_options.y = float(window_->rendered_frames() % 4093);
        history.epoch          = window_->history_epoch();
        history.camera         = camera;
        history.vp             = frame.view_projection;
        history.scene          = current_scene_;
        history.frame          = window_->rendered_frames();
        window_->report_history(history.age);
    } else {
        history.age = 0;
        history.scene.reset();
    }
    frame.area_settings =
        glm::vec4(static_cast<float>(window_->scene()->rectangles.size()),
                  static_cast<float>(area_shadows_.grid()), 1.F / RectangleShadow::resolution, 0);
    frame.teaching_light   = window_->teaching_light();
    frame.teaching_surface = glm::vec4(window_->teaching_surface(), 0.0F);
    if (window_->comparison_mode() < 0) {
        uniforms_.write(&frame, sizeof(frame));
        return;
    }
    // 两侧在同一个命令缓冲中连续绘制。将参数快照录入命令流，保持共同相机，
    // 同时让每个绘制读取自己的显示模式。附件复用前等待前一侧读写完成。
    const auto      cmd = window_->currentCommandBuffer();
    VkMemoryBarrier before{};
    before.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    before.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    before.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
    vkCmdUpdateBuffer(cmd, uniforms_.handle(), 0, sizeof(frame), &frame);
    VkMemoryBarrier ready{};
    ready.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    ready.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    ready.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 1, &ready, 0, nullptr, 0, nullptr);
}

void SceneRenderer::render_shadow_maps(VkCommandBuffer command, int mode) {
    shadow_map_.prepare(command);
    if (!window_->shadows_enabled() || (mode == VulkanViewport::base_color_preview_mode ||
                                        mode == VulkanViewport::blinn_phong_mode)) {
        return;
    }
    // 阴影图按场景修订号缓存。灯光或几何变动由 upload_scene 使缓存失效。
    if (shadow_depth_ready_) {
        return;
    }
    shadow_depth_ready_ = true;
    for (std::int32_t light = 0;
         light < static_cast<std::int32_t>(window_->scene()->directional_lights.size()); ++light) {
        shadow_map_.begin(command, static_cast<std::uint32_t>(light));
        draw_shadow_geometry(command, light, ShadowMap::resolution);
        vkCmdEndRenderPass(command);
    }
}

int SceneRenderer::area_grid() const {
    if (!window_->area_shadows() || window_->scene()->rectangles.empty() ||
        (!window_->uses_lighting(window_->view_mode()) &&
         !window_->uses_lighting(window_->comparison_mode()))) {
        return 0;
    }
    return window_->navigating() ? 2 : 3;
}

void SceneRenderer::draw_shadow_geometry(VkCommandBuffer command, int projection,
                                         std::uint32_t resolution, const std::string* emitter) {
    const VkViewport viewport{0, 0, static_cast<float>(resolution), static_cast<float>(resolution),
                              0, 1};
    const VkRect2D   scissor{{0, 0}, {resolution, resolution}};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1,
                            &descriptor_, 0, nullptr);
    for (std::size_t index = 0; index < draws_.size(); ++index) {
        if (emitter && window_->scene()->instances[index].object_id == *emitter) {
            continue;
        }
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          shadow_pipelines_[side_pipeline(window_->scene()->instances[index])]);
        const DrawIndex push{projection, static_cast<std::int32_t>(index)};
        vkCmdPushConstants(command, pipeline_layout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        const auto&        mesh   = meshes_.at(draws_[index].mesh);
        const VkDeviceSize offset = 0;
        const auto         buffer = mesh.vertices.handle();
        vkCmdBindVertexBuffers(command, 0, 1, &buffer, &offset);
        vkCmdDraw(command, mesh.count, 1, 0, 0);
    }
}

void SceneRenderer::render_wire(VkCommandBuffer command, int mode) {
    // 后处理始终绑定 wire 图像，首次先清除使其布局有效；以后仅在线框模式绘制。
    if (mode != 3 && targets_.wire_initialized()) {
        return;
    }
    targets_.begin_wire(command);
    if (mode == 3) {
        const auto       size = window_->swapChainImageSize();
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
            const auto&        mesh   = meshes_.at(draws_[index].mesh);
            const auto         buffer = mesh.vertices.handle();
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

bool SceneRenderer::update_prt() {
    const auto data = window_->prt();
    if (prt_buffer_.handle() && uploaded_prt_ == data) {
        return false;
    }
    const auto             count = data ? data->vertices.size() * 9 : 0;
    std::vector<glm::vec4> packed(std::max<std::size_t>(1, count));
    if (data) {
        for (std::size_t i = 0; i < data->vertices.size(); ++i) {
            for (std::size_t k = 0; k < 9; ++k) {
                packed[i * 9 + k] = glm::vec4(data->vertices[i][k], 0);
            }
        }
    }
    prt_buffer_.create(device_, window_->physicalDevice(), packed.size() * sizeof(glm::vec4),
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    prt_buffer_.write(packed.data(), packed.size() * sizeof(glm::vec4));
    uploaded_prt_ = data;
    return true;
}

void SceneRenderer::startNextFrame() {
    if ((window_->view_mode() >= 19 && window_->view_mode() <= 20) ||
        (window_->comparison_mode() >= 19 && window_->comparison_mode() <= 20)) {
        ssao_.prepare_temporal(*window_);
    }
    const bool ibl_changed = ibl_.update(*window_, window_->ibl_maps(), window_->ibl_lut());
    if (scene_revision_ != window_->scene_revision()) {
        upload_scene();
    }
    if (update_prt()) {
        rebuild_texture_descriptors();
    }
    if (area_shadows_.configure(*window_, *window_->scene(), minimum_, maximum_, area_grid(),
                                window_->scene_revision())) {
        rebuild_texture_descriptors();
        comparison_keys_.fill({0, -1});
    }
    if (ibl_changed) {
        update_ibl_descriptors();
        window_->report_ibl_update();
    }
    ibl_.record_upload(window_->currentCommandBuffer());
    rectangles_.record_upload(window_->currentCommandBuffer());
    for (auto& [handle, texture] : textures_) {
        texture->retire_upload();
        texture->record_upload(window_->currentCommandBuffer());
    }
    const auto command = window_->currentCommandBuffer();
    area_shadows_.render(command, [&](std::uint32_t layer, std::size_t source) {
        draw_shadow_geometry(command, -1 - static_cast<int>(layer), RectangleShadow::resolution,
                             &window_->scene()->rectangles[source].object_id);
    });
    window_->report_area_shadow(area_shadows_.grid(), area_shadows_.updates());
    const auto overlay = [this] {
        if (window_->export_side() < 0) {
            gizmo_.draw(*window_, window_->gizmo_layout(), window_->highlighted_gizmo_handle());
        }
    };
    if (window_->comparison_mode() < 0) {
        render_view(window_->view_mode(), false);
        if (capture_hdr_) {
            targets_.record_readback(window_->currentCommandBuffer(), ssao_.hdr_image());
        }
        post_process_.draw(*window_, overlay);
    } else {
        post_process_.prepare_comparison(*window_);
        const std::array modes{window_->view_mode(), window_->comparison_mode()};
        for (std::size_t side = 0; side < modes.size(); ++side) {
            const auto key =
                std::make_pair(window_->comparison_revision(modes[side], side == 1), modes[side]);
            const bool capture =
                capture_hdr_ && static_cast<int>(side) == std::max(0, window_->export_side());
            if (comparison_keys_[side] == key && !capture &&
                ((modes[side] != 19 && modes[side] != 20) || histories_[side].age >= 32)) {
                continue;
            }
            render_view(modes[side], side == 1);
            window_->report_comparison_draw();
            if (capture) {
                targets_.record_readback(window_->currentCommandBuffer(), ssao_.hdr_image());
            }
            post_process_.store_side(*window_, side, modes[side]);
            comparison_keys_[side] = key;
        }
        post_process_.present_comparison(*window_, overlay);
    }
    capture_hdr_ = false;
    window_->report_frame();
    window_->frameReady();
    window_->requestUpdate();
}

void SceneRenderer::render_view(int mode, bool reference) {
    update_uniforms(mode, reference);
    render_shadow_maps(window_->currentCommandBuffer(), mode);
    const auto       size = window_->swapChainImageSize();
    const VkExtent2D extent{static_cast<std::uint32_t>(size.width()),
                            static_cast<std::uint32_t>(size.height())};
    const auto       command = window_->currentCommandBuffer();
    targets_.begin(command);
    const VkViewport viewport{
        0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height), 0, 1};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    if (window_->ibl_maps() && window_->environment_background() &&
        mode != VulkanViewport::base_color_preview_mode) {
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
        const auto&        mesh   = meshes_.at(draw.mesh);
        const VkDeviceSize offset = 0;
        const auto         buffer = mesh.vertices.handle();
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
    render_wire(command, mode);
    ssao_.draw(*window_, mode, reference);
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

void SceneRenderer::request_hdr_capture() {
    capture_hdr_ = true;
}

minirender::Image SceneRenderer::read_hdr_capture() {
    return targets_.readback();
}
