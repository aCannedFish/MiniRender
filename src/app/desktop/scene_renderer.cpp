// 调度帧资源更新，录制阴影、几何与合成通道，并发布显示图或 HDR 读回。

#include "scene_renderer.hpp"
#include "scene_gpu_layout.hpp"
#include <algorithm>

using scene_gpu::DrawIndex;

namespace {
// 镜像改变屏幕绕序，切换 frontFace 后仍以原网格外侧作为正面。
std::size_t side_pipeline(const minirender::RenderInstance& instance) {
    if (instance.material.double_sided) {
        return 0;
    }
    return glm::determinant(glm::mat3(instance.object_to_world)) < 0 ? 2 : 1;
}

} // namespace

SceneRenderer::SceneRenderer(VulkanViewport* window) : window_(window) {}

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
    if (mode != VIEW_WIRE && targets_.wire_initialized()) {
        return;
    }
    targets_.begin_wire(command);
    if (mode == VIEW_WIRE) {
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

void SceneRenderer::startNextFrame() {
    if (VulkanViewport::uses_history(window_->view_mode()) ||
        VulkanViewport::uses_history(window_->comparison_mode())) {
        ssao_.prepare_temporal(*window_);
    }
    if (uploaded_reference_ != window_->reference_image()) {
        uploaded_reference_ = window_->reference_image();
        const minirender::Image black(1, 1);
        reference_texture_.create(*window_,
                                  {uploaded_reference_ ? uploaded_reference_.get() : &black, 1});
        ssao_.set_reference(reference_texture_.descriptor().imageView);
    }
    reference_texture_.retire_upload();
    reference_texture_.record_upload(window_->currentCommandBuffer());
    const bool ibl_changed = ibl_.update(*window_, window_->ibl_maps(), window_->ibl_lut());
    if (scene_revision_ != window_->scene_revision()) {
        upload_scene();
    }
    if (update_prt()) {
        rebuild_texture_descriptors();
    }
    if (area_shadows_.grid() != area_grid()) {
        comparison_keys_.fill({0, -1});
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
                (!VulkanViewport::uses_history(modes[side]) || histories_[side].age >= 32)) {
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

void SceneRenderer::request_hdr_capture() {
    capture_hdr_ = true;
}

minirender::Image SceneRenderer::read_hdr_capture() {
    return targets_.readback();
}
