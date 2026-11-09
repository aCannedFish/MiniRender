#include <minirender/scene/editor.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace minirender {

SceneEditor::SceneEditor(SceneDocument document) : document_(std::move(document)) {
    document_.validate(&validated_meshes_);
}

const SceneDocument& SceneEditor::document() const noexcept {
    return document_;
}

void SceneEditor::replace(SceneDocument document) {
    document.validate(&validated_meshes_);
    document_ = std::move(document);
    transform_edit_.reset();
    undo_.clear();
    redo_.clear();
}

void SceneEditor::replace_prevalidated(SceneDocument document,
                                       const MeshValidationCache& validated) {
    validated_meshes_.merge_validated(validated);
    replace(std::move(document));
}

void SceneEditor::edit(const std::function<void(SceneDocument&)>& operation) {
    SceneDocument candidate = document_;
    operation(candidate);
    candidate.validate(&validated_meshes_);
    commit_transform_edit();
    undo_.push_back(document_);
    if (undo_.size() > 100) {
        undo_.erase(undo_.begin());
    }
    document_ = std::move(candidate);
    redo_.clear();
}

void SceneEditor::edit_prevalidated(const std::function<void(SceneDocument&)>& operation,
                                    const MeshValidationCache& validated) {
    validated_meshes_.merge_validated(validated);
    edit(operation);
}

bool SceneEditor::undo() {
    if (transform_edit_ && cancel_transform_edit()) {
        return true;
    }
    if (undo_.empty()) {
        return false;
    }
    redo_.push_back(std::move(document_));
    document_ = std::move(undo_.back());
    undo_.pop_back();
    return true;
}

bool SceneEditor::redo() {
    cancel_transform_edit();
    if (redo_.empty()) {
        return false;
    }
    undo_.push_back(std::move(document_));
    document_ = std::move(redo_.back());
    redo_.pop_back();
    return true;
}

bool SceneEditor::can_undo() const noexcept {
    return (transform_edit_ && transform_edit_->changed) || !undo_.empty();
}
bool SceneEditor::can_redo() const noexcept {
    return !(transform_edit_ && transform_edit_->changed) && !redo_.empty();
}

void SceneEditor::begin_transform_edit(const std::string& object_id) {
    if (transform_edit_) {
        throw std::logic_error("A transform gesture is already active");
    }
    const auto object = std::find_if(document_.objects.begin(), document_.objects.end(),
                                     [&](const auto& item) { return item.id == object_id; });
    if (object == document_.objects.end()) {
        throw std::invalid_argument("Transform target is missing");
    }
    transform_edit_ = TransformEdit{object_id, document_, false};
}

void SceneEditor::preview_transform(const Transform& transform) {
    if (!transform_edit_) {
        throw std::logic_error("Begin a transform gesture before updating its preview");
    }
    transform.validate();
    auto candidate = document_;
    const auto target =
        std::find_if(candidate.objects.begin(), candidate.objects.end(),
                     [&](const auto& item) { return item.id == transform_edit_->object_id; });
    const auto original = std::find_if(
        transform_edit_->initial.objects.begin(), transform_edit_->initial.objects.end(),
        [&](const auto& item) { return item.id == transform_edit_->object_id; });
    if (target == candidate.objects.end() || original == transform_edit_->initial.objects.end()) {
        throw std::logic_error("Transform target changed during a gesture");
    }
    target->transform = transform;
    candidate.validate(&validated_meshes_);
    transform_edit_->changed = transform.translation != original->transform.translation ||
                               transform.rotation_degrees != original->transform.rotation_degrees ||
                               transform.scale != original->transform.scale;
    document_ = std::move(candidate);
}

bool SceneEditor::commit_transform_edit() {
    if (!transform_edit_) {
        return false;
    }
    const bool changed = transform_edit_->changed;
    if (changed) {
        undo_.push_back(std::move(transform_edit_->initial));
        if (undo_.size() > 100) {
            undo_.erase(undo_.begin());
        }
        redo_.clear();
    }
    transform_edit_.reset();
    return changed;
}

bool SceneEditor::cancel_transform_edit() {
    if (!transform_edit_) {
        return false;
    }
    const bool changed = transform_edit_->changed;
    document_ = std::move(transform_edit_->initial);
    transform_edit_.reset();
    return changed;
}

bool SceneEditor::transforming() const noexcept {
    return transform_edit_.has_value();
}

void SceneEditor::update_navigation(const Camera& camera) {
    camera.validate();
    document_.camera = camera; // 连续导航不为每个 mouse move 生成撤销项。
    if (transform_edit_) {
        transform_edit_->initial.camera = camera;
    }
}

std::shared_ptr<const SceneSnapshot> SceneEditor::snapshot() {
    return make_snapshot(document_, &validated_meshes_);
}

Camera rotate_camera(Camera camera, float yaw_delta, float pitch_delta, bool orbit) {
    camera.validate();
    const auto up = glm::normalize(camera.up);
    const auto direction = glm::normalize(camera.target - camera.position);
    const float distance = glm::length(camera.target - camera.position);
    const float old_pitch = std::asin(std::clamp(glm::dot(direction, up), -1.0F, 1.0F));
    const float pitch =
        std::clamp(old_pitch + pitch_delta, glm::radians(-85.0F), glm::radians(85.0F));
    auto horizontal = glm::normalize(direction - glm::dot(direction, up) * up);
    horizontal = glm::mat3(glm::rotate(glm::mat4(1), yaw_delta, up)) * horizontal;
    const auto forward = std::cos(pitch) * horizontal + std::sin(pitch) * up;
    if (orbit) {
        camera.position = camera.target - distance * forward;
    } else {
        camera.target = camera.position + distance * forward;
    }
    return camera;
}

Camera pan_camera(Camera camera, glm::vec2 displacement) {
    return move_camera(camera, {displacement.x, displacement.y, 0});
}

Camera move_camera(Camera camera, glm::vec3 local_displacement) {
    camera.validate();
    const auto forward = glm::normalize(camera.target - camera.position);
    const auto right = glm::normalize(glm::cross(forward, camera.up));
    const auto up = glm::cross(right, forward);
    const auto delta =
        right * local_displacement.x + up * local_displacement.y + forward * local_displacement.z;
    camera.position += delta;
    camera.target += delta;
    return camera;
}

Camera dolly_camera(Camera camera, float distance_factor) {
    camera.validate();
    const auto delta = camera.position - camera.target;
    const float distance = std::clamp(glm::length(delta) * distance_factor, 0.1F, 1000.0F);
    camera.position = camera.target + glm::normalize(delta) * distance;
    return camera;
}

std::optional<std::string> pick_object(const SceneSnapshot& scene, const Ray& ray) {
    double nearest = std::numeric_limits<double>::infinity();
    std::optional<std::string> selected;
    for (const auto& instance : scene.instances) {
        for (const auto& triangle : instance.mesh->triangles) {
            std::array<glm::dvec3, 3> p;
            for (std::size_t k = 0; k < 3; ++k) {
                p[k] =
                    glm::vec3(instance.object_to_world *
                              glm::vec4(instance.mesh->positions[triangle.corners[k].position], 1));
            }
            // Möller–Trumbore，双面求交；t 采用世界空间单位以比较不同缩放的实例。
            const auto edge1 = p[1] - p[0];
            const auto edge2 = p[2] - p[0];
            const auto cross = glm::cross(ray.direction, edge2);
            const double determinant = glm::dot(edge1, cross);
            if (std::abs(determinant) < 1e-9F) {
                continue;
            }
            const double inverse = 1.0 / determinant;
            const auto offset = ray.origin - p[0];
            const double u = glm::dot(offset, cross) * inverse;
            const auto q = glm::cross(offset, edge1);
            const double v = glm::dot(ray.direction, q) * inverse;
            const double t = glm::dot(edge2, q) * inverse;
            if (u >= 0 && v >= 0 && u + v <= 1 && t > 1e-5F && t < nearest) {
                nearest = t;
                selected = instance.object_id;
            }
        }
    }
    return selected;
}

} // namespace minirender
