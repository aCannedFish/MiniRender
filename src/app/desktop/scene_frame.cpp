// 打包相机与材质显示参数，判定时域历史有效性并同步本次绘制的 UBO。

#include "scene_gpu_layout.hpp"
#include "scene_renderer.hpp"
#include "vulkan_utils.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

using scene_gpu::FrameUniform;

void SceneRenderer::update_uniforms(int mode, bool reference) {
    const auto   size   = window_->swapChainImageSize();
    const auto&  camera = window_->camera();
    FrameUniform frame{};
    // 视口尺寸为物理像素。所有几何和屏幕空间通道读取同一组相机矩阵。
    frame.view            = camera.view_matrix();
    frame.projection      = camera.projection_matrix(static_cast<float>(size.width()) /
                                                     static_cast<float>(size.height()));
    frame.view_projection = frame.projection * frame.view;
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
    // 每侧历史独立检查相机切换、场景刚体变换和上一帧序号。
    auto& history = histories_[reference ? 1 : 0];
    if (VulkanViewport::uses_history(mode)) {
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
