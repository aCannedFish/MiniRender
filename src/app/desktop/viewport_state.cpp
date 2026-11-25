// 管理场景快照、预览模式和参数变化，通知渲染器更新对应的缓存。

#include "vulkan_viewport.hpp"
#include <QScopedValueRollback>
#include <algorithm>
#include <cmath>
#include <stdexcept>

void VulkanViewport::set_scene(std::shared_ptr<const minirender::SceneSnapshot> scene) {
    if (scene && scene->directional_lights.size() > 8) {
        throw std::invalid_argument("Realtime preview supports at most 8 directional lights");
    }
    if (static_cast<bool>(scene_) != static_cast<bool>(scene) && device_ != VK_NULL_HANDLE) {
        release();
    }
    if (!scene_ || !scene || scene_->environment_texture != scene->environment_texture) {
        ibl_maps_.reset();
    }
    if (prt_ && (!scene || !scene_ || !minirender::compatible_prt(*scene_, *scene, prt_kind_))) {
        prt_.reset();
    }
    if (path_reference_ && scene != scene_) {
        set_reference_image({});
    }
    scene_ = std::move(scene);
    raster_meshes_.retain(scene_ ? std::span<const minirender::RenderInstance>(scene_->instances)
                                 : std::span<const minirender::RenderInstance>{});
    if (scene_) {
        if (point_visible_ || sdf_visible_) {
            scene_camera_before_tools_ = scene_->camera;
        } else {
            camera_ = scene_->camera;
        }
    }
    rebuild_gizmo();
    ++scene_revision_;
    mark_preview_changed();
    requestUpdate();
}

const std::shared_ptr<const minirender::SceneSnapshot>& VulkanViewport::scene() const noexcept {
    return scene_;
}

std::uint64_t VulkanViewport::scene_revision() const noexcept {
    return scene_revision_;
}

void VulkanViewport::supply_raster_mesh(
    const std::shared_ptr<const minirender::MeshData>& mesh,
    std::shared_ptr<const minirender::RasterMeshData>  prepared) {
    raster_meshes_.supply(mesh, std::move(prepared));
}

std::shared_ptr<const minirender::RasterMeshData>
VulkanViewport::raster_mesh(const std::shared_ptr<const minirender::MeshData>& mesh) {
    return raster_meshes_.find(mesh);
}

const minirender::Camera& VulkanViewport::camera() const noexcept {
    return camera_;
}

void VulkanViewport::set_camera(const minirender::Camera& camera) {
    camera.validate();
    finish_gizmo_drag(true);
    if (path_reference_ &&
        (camera.position != camera_.position || camera.target != camera_.target ||
         camera.up != camera_.up || camera.vertical_fov_degrees != camera_.vertical_fov_degrees ||
         camera.aperture_radius != camera_.aperture_radius ||
         camera.focus_distance != camera_.focus_distance ||
         camera.near_plane != camera_.near_plane || camera.far_plane != camera_.far_plane)) {
        set_reference_image({});
    }
    camera_ = camera;
    rebuild_gizmo();
    mark_preview_changed();
    if (on_camera_changed && !point_visible_ && !sdf_visible_) {
        on_camera_changed(camera_);
    }
    requestUpdate();
}

void VulkanViewport::set_point_preview_data(
    std::shared_ptr<const minirender::PointPreviewData> data) {
    if (data && data->points.empty()) {
        throw std::invalid_argument("Point preview data must contain points");
    }
    if (!data) {
        set_point_preview_visible(false);
    }
    const bool first = !point_data_;
    point_data_      = std::move(data);
    if (first && point_data_) {
        reset_point_camera();
    }
    mark_preview_changed();
    requestUpdate();
}

const std::shared_ptr<const minirender::PointPreviewData>&
VulkanViewport::point_preview_data() const noexcept {
    return point_data_;
}

void VulkanViewport::set_point_preview_settings(minirender::PointPreviewSettings settings) {
    settings.validate();
    point_settings_ = settings;
    mark_preview_changed();
    requestUpdate();
}

const minirender::PointPreviewSettings& VulkanViewport::point_preview_settings() const noexcept {
    return point_settings_;
}

void VulkanViewport::set_point_preview_visible(bool visible) {
    if (point_visible_ == visible) {
        return;
    }
    if (visible && !point_data_) {
        throw std::logic_error("Load a point cloud before opening its preview");
    }
    finish_gizmo_drag(false);
    if (visible) {
        set_sdf_visible(false);
        set_offline_visible(false);
        scene_camera_before_tools_ = camera_;
        camera_                    = point_camera_;
    } else {
        point_camera_ = camera_;
        camera_       = scene_camera_before_tools_;
    }
    point_visible_   = visible;
    pressed_buttons_ = Qt::NoButton;
    dragged_         = false;
    rebuild_gizmo();
    mark_preview_changed();
    if (on_point_preview_changed) {
        on_point_preview_changed(visible);
    }
    requestUpdate();
}

bool VulkanViewport::point_preview_visible() const noexcept {
    return point_visible_;
}

void VulkanViewport::reset_point_camera() {
    minirender::Camera camera;
    camera.position   = {3, 2, 3};
    camera.target     = {0, 0, 0};
    camera.near_plane = .01F;
    camera.far_plane  = 100;
    point_camera_     = camera;
    if (point_visible_) {
        set_camera(camera);
    }
}

void VulkanViewport::report_point_upload() {
    ++point_uploads_;
}

std::uint64_t VulkanViewport::point_uploads() const noexcept {
    return point_uploads_;
}

void VulkanViewport::set_sdf_document(std::shared_ptr<const minirender::SdfDocument> document,
                                      bool                                           reset_camera) {
    if (document) {
        document->validate();
    } else {
        set_sdf_visible(false);
    }
    if (document && (!sdf_document_ || reset_camera)) {
        sdf_camera_ = document->camera;
        if (sdf_visible_) {
            set_camera(sdf_camera_);
        }
    }
    sdf_document_ = std::move(document);
    mark_preview_changed();
    requestUpdate();
}

const std::shared_ptr<const minirender::SdfDocument>&
VulkanViewport::sdf_document() const noexcept {
    return sdf_document_;
}

void VulkanViewport::set_sdf_visible(bool visible) {
    if (visible == sdf_visible_) {
        return;
    }
    if (visible && !sdf_document_) {
        throw std::logic_error("Load an SDF document before opening its view");
    }
    finish_gizmo_drag(false);
    if (visible) {
        set_point_preview_visible(false);
        set_offline_visible(false);
        scene_camera_before_tools_ = camera_;
        camera_                    = sdf_camera_;
    } else {
        sdf_camera_ = camera_;
        camera_     = scene_camera_before_tools_;
    }
    sdf_visible_     = visible;
    pressed_buttons_ = Qt::NoButton;
    dragged_         = false;
    rebuild_gizmo();
    mark_preview_changed();
    if (on_sdf_visible_changed) {
        on_sdf_visible_changed(visible);
    }
    requestUpdate();
}

bool VulkanViewport::sdf_visible() const noexcept {
    return sdf_visible_;
}

void VulkanViewport::set_sdf_view(minirender::SdfView view) {
    if (view != minirender::SdfView::surface && view != minirender::SdfView::normals &&
        view != minirender::SdfView::depth && view != minirender::SdfView::steps) {
        throw std::invalid_argument("Unknown SDF view");
    }
    sdf_view_ = view;
    if (on_sdf_display_changed) {
        on_sdf_display_changed();
    }
    mark_preview_changed();
    requestUpdate();
}

minirender::SdfView VulkanViewport::sdf_view() const noexcept {
    return sdf_view_;
}

void VulkanViewport::set_sdf_shadows(bool enabled) {
    sdf_shadows_ = enabled;
    if (on_sdf_display_changed) {
        on_sdf_display_changed();
    }
    mark_preview_changed();
    requestUpdate();
}

bool VulkanViewport::sdf_shadows() const noexcept {
    return sdf_shadows_;
}

const minirender::Camera& VulkanViewport::sdf_camera() const noexcept {
    return sdf_visible_ ? camera_ : sdf_camera_;
}

void VulkanViewport::report_sdf_upload() {
    ++sdf_uploads_;
}

std::uint64_t VulkanViewport::sdf_uploads() const noexcept {
    return sdf_uploads_;
}

void VulkanViewport::set_exposure(float value) {
    exposure_ = value;
    mark_preview_changed();
    requestUpdate();
}

float VulkanViewport::exposure() const noexcept {
    return exposure_;
}

void VulkanViewport::set_preview_outline(bool enabled) {
    preview_outline_ = enabled;
    mark_preview_changed();
    requestUpdate();
}

bool VulkanViewport::preview_outline() const noexcept {
    return preview_outline_;
}

void VulkanViewport::set_preview_outline_strength(float strength) {
    if (!std::isfinite(strength) || strength < 0 || strength > 1) {
        throw std::invalid_argument("Preview outline strength must be in [0, 1]");
    }
    preview_outline_strength_ = strength;
    mark_preview_changed();
    requestUpdate();
}

float VulkanViewport::preview_outline_strength() const noexcept {
    return preview_outline_strength_;
}

void VulkanViewport::set_preview_occlusion(bool enabled) {
    preview_occlusion_ = enabled;
    mark_preview_changed();
    requestUpdate();
}

bool VulkanViewport::preview_occlusion() const noexcept {
    return preview_occlusion_;
}

void VulkanViewport::set_prt(std::shared_ptr<const minirender::PrtBake> bake,
                             minirender::PrtMode                        mode) {
    prt_      = std::move(bake);
    prt_kind_ = mode;
    mark_preview_changed();
    requestUpdate();
}

void VulkanViewport::set_prt_lighting(const minirender::ShLight& lighting) {
    prt_lighting_ = lighting;
    mark_preview_changed();
    requestUpdate();
}

void VulkanViewport::reset_history() {
    ++history_epoch_;
    mark_preview_changed();
    requestUpdate();
}

void VulkanViewport::set_temporal_settings(glm::vec2 value) {
    temporal_ = value;
    reset_history();
}

void VulkanViewport::set_screen_trace(glm::vec4 settings, bool hiz) {
    ++history_epoch_;
    screen_trace_ = settings;
    screen_hiz_   = hiz;
    mark_preview_changed();
    requestUpdate();
}

void VulkanViewport::set_teaching(glm::vec4 light, glm::vec3 surface) {
    teaching_light_   = light;
    teaching_surface_ = surface;
    ++teaching_revision_;
    requestUpdate();
}

void VulkanViewport::set_comparison_mode(int mode) {
    comparison_mode_ = mode;
    dragging_split_  = false;
    requestUpdate();
}

void VulkanViewport::set_split_position(float value) {
    split_ = std::clamp(value, 0.0F, 1.0F);
    requestUpdate();
}

QImage VulkanViewport::grab_comparison_side(int side) {
    QScopedValueRollback<int> restore(export_side_, side);
    return grab();
}

minirender::Image VulkanViewport::grab_comparison_hdr(int side) {
    if (side == 1 && comparison_mode_ == offline_reference_mode && path_reference_) {
        return *path_reference_;
    }
    QScopedValueRollback<int> restore(export_side_, side);
    return grab_hdr();
}

void VulkanViewport::set_view_mode(int mode) {
    set_sdf_visible(false);
    set_point_preview_visible(false);
    if (view_mode_ == mode) {
        return;
    }
    view_mode_ = mode;
    if (comparison_mode_ < 0) {
        mark_preview_changed();
    }
    requestUpdate();
}

int VulkanViewport::view_mode() const noexcept {
    return view_mode_;
}

void VulkanViewport::set_ssao_settings(minirender::SsaoSettings settings) {
    settings.validate();
    ssao_ = settings;
    mark_preview_changed();
    requestUpdate();
}

const minirender::SsaoSettings& VulkanViewport::ssao_settings() const noexcept {
    return ssao_;
}

void VulkanViewport::set_ibl_data(std::shared_ptr<const minirender::IblEnvironment> maps,
                                  std::shared_ptr<const minirender::Image>          lut) {
    if (maps == ibl_maps_ && lut == ibl_lut_) {
        return;
    }
    ++history_epoch_;
    ibl_maps_ = std::move(maps);
    ibl_lut_  = std::move(lut);
    mark_preview_changed();
    requestUpdate();
}

const std::shared_ptr<const minirender::IblEnvironment>& VulkanViewport::ibl_maps() const noexcept {
    return ibl_maps_;
}

const std::shared_ptr<const minirender::Image>& VulkanViewport::ibl_lut() const noexcept {
    return ibl_lut_;
}

void VulkanViewport::set_environment_background(bool enabled) {
    environment_background_ = enabled;
    mark_preview_changed();
    requestUpdate();
}

bool VulkanViewport::environment_background() const noexcept {
    return environment_background_;
}

void VulkanViewport::report_ibl_update() {
    ++ibl_texture_updates_;
}

std::uint64_t VulkanViewport::ibl_texture_updates() const noexcept {
    return ibl_texture_updates_;
}

void VulkanViewport::set_offline_visible(bool visible) {
    if (visible) {
        set_sdf_visible(false);
        set_point_preview_visible(false);
    }
    if (offline_visible_ == visible) {
        return;
    }
    finish_gizmo_drag(true);
    ++display_revision_;
    offline_visible_ = visible;
    rebuild_gizmo();
    pressed_buttons_ = Qt::NoButton;
    dragged_         = false;
    mark_preview_changed();
    if (on_display_changed) {
        on_display_changed(visible);
    }
    requestUpdate();
}

bool VulkanViewport::offline_visible() const noexcept {
    return offline_visible_;
}

OfflineImageSource VulkanViewport::offline_source() const noexcept {
    return offline_source_;
}

void VulkanViewport::set_offline_image(std::shared_ptr<const minirender::Image> image,
                                       OfflineImageSource                       source) {
    if (source != offline_source_ || !image) {
        ++display_revision_;
    }
    offline_source_ = source;
    offline_image_  = std::move(image);
    mark_preview_changed();
    requestUpdate();
}

const std::shared_ptr<const minirender::Image>& VulkanViewport::offline_image() const noexcept {
    return offline_image_;
}

void VulkanViewport::set_area_shadows(bool enabled) {
    ++history_epoch_;
    area_shadows_ = enabled;
    mark_preview_changed();
    requestUpdate();
}

void VulkanViewport::set_shadows_enabled(bool enabled) {
    ++history_epoch_;
    shadows_enabled_ = enabled;
    mark_preview_changed();
    requestUpdate();
}

bool VulkanViewport::shadows_enabled() const noexcept {
    return shadows_enabled_;
}

void VulkanViewport::set_shadow_filter(ShadowFilter filter) {
    ++history_epoch_;
    shadow_filter_ = filter;
    mark_preview_changed();
    requestUpdate();
}

ShadowFilter VulkanViewport::shadow_filter() const noexcept {
    return shadow_filter_;
}

void VulkanViewport::set_light_half_angle(float degrees) {
    ++history_epoch_;
    light_half_angle_ = std::clamp(degrees, 0.0F, 15.0F);
    mark_preview_changed();
    requestUpdate();
}

float VulkanViewport::light_half_angle() const noexcept {
    return light_half_angle_;
}

void VulkanViewport::set_shadow_filter_radius(float texels) {
    ++history_epoch_;
    shadow_filter_radius_ = std::clamp(texels, 0.0F, 48.0F);
    mark_preview_changed();
    requestUpdate();
}

float VulkanViewport::shadow_filter_radius() const noexcept {
    return shadow_filter_radius_;
}

void VulkanViewport::set_reference_image(std::shared_ptr<const minirender::Image> image) {
    path_reference_ = std::move(image);
    ++reference_revision_;
    requestUpdate();
}

void VulkanViewport::set_reference_energy(int value) {
    ++history_epoch_;
    reference_energy_ = value;
    ++reference_revision_;
    requestUpdate();
}

void VulkanViewport::set_reference_shadow(int filter) {
    ++history_epoch_;
    reference_shadow_ = filter;
    ++reference_revision_;
    requestUpdate();
}

void VulkanViewport::set_shadow_debug_light(int light) {
    shadow_debug_light_ = std::clamp(light, 0, 7);
    mark_preview_changed();
    requestUpdate();
}

void VulkanViewport::set_fxaa_enabled(bool enabled) {
    fxaa_enabled_ = enabled;
    mark_preview_changed();
    requestUpdate();
}

bool VulkanViewport::fxaa_enabled() const noexcept {
    return fxaa_enabled_;
}
