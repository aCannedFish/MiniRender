// 将场景快照上传为实例、网格、PRT 和纹理缓冲，并维护描述符绑定。

#include "scene_gpu_layout.hpp"
#include "scene_renderer.hpp"
#include "vulkan_utils.hpp"
#include <QElapsedTimer>
#include <algorithm>
#include <bit>
#include <minirender/scene/shadow.hpp>
#include <set>

using namespace scene_gpu;
using Vertex = minirender::RasterVertex;

// 若源已在缓存中，多个实例共享同一份缓冲；否则优先接管已移除的同尺寸网格缓冲。
const SceneRenderer::Mesh& SceneRenderer::upload_mesh(const MeshHandle& source,
                                                      const MeshSet&    used) {
    const auto existing = meshes_.find(source);
    if (existing != meshes_.end()) {
        return existing->second;
    }
    Mesh mesh;
    auto prepared = window_->raster_mesh(source);
    if (!prepared) {
        // 同步诊断入口现场准备；交互导入与几何任务在后台完成准备。
        prepared = minirender::prepare_raster_mesh(*source);
        window_->supply_raster_mesh(source, prepared);
    }
    const auto count    = prepared->vertices.size();
    const auto reusable = std::find_if(meshes_.begin(), meshes_.end(), [&](const auto& entry) {
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
        mesh.edges.write(prepared->edges.data(), prepared->edges.size() * sizeof(std::uint32_t));
        window_->report_mesh_allocation();
    }
    mesh.count   = static_cast<std::uint32_t>(count);
    mesh.minimum = prepared->minimum;
    mesh.maximum = prepared->maximum;
    mesh.vertices.write(prepared->vertices.data(), count * sizeof(Vertex));
    const auto inserted = meshes_.emplace(source, std::move(mesh)).first;
    window_->report_mesh_upload();
    return inserted->second;
}

void SceneRenderer::upload_scene() {
    shadow_depth_ready_ = false;
    QElapsedTimer timer;
    timer.start();
    const auto& scene = *window_->scene();
    subsurface_previews_.begin_scene();
    MeshSet                  used;
    std::vector<Draw>        draws;
    std::vector<GpuInstance> instances;
    glm::vec3                minimum(std::numeric_limits<float>::infinity());
    glm::vec3                maximum(-std::numeric_limits<float>::infinity());
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
    // 先收集全部可见来源，避免复用仍被其他实例引用的网格缓冲。
    std::uint32_t corner_offset = 0;
    for (const auto& instance : scene.instances) {
        used.insert(instance.mesh);
    }
    for (const auto& instance : scene.instances) {
        const auto& mesh = upload_mesh(instance.mesh, used);
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
