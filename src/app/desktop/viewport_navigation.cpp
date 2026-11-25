// 处理鼠标与键盘导航、物体拾取、操纵器拖动以及分屏手势。

#include "vulkan_viewport.hpp"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <minirender/scene/editor.hpp>

void VulkanViewport::mousePressEvent(QMouseEvent* event) {
    if (offline_visible_) {
        event->accept();
        return;
    }
    if (comparison_mode_ >= 0 && !point_visible_ && !sdf_visible_ &&
        event->button() == Qt::LeftButton &&
        std::abs(event->position().x() - split_ * static_cast<float>(width())) <= 7.0) {
        dragging_split_ = true;
        event->accept();
        return;
    }
    // 嵌入窗口的焦点由 QWidget 容器管理，避免把子窗口当顶层窗口激活。
    pressed_buttons_ = event->buttons();
    mouse_origin_ = last_mouse_ = event->position();
    dragged_                    = false;
    if (event->button() == Qt::LeftButton && event->modifiers() == Qt::NoModifier &&
        gizmo_target_) {
        const glm::dvec2            pointer(event->position().x(), event->position().y());
        const auto                  hit = gizmo_layout_.hit(pointer);
        const minirender::GizmoView view{camera_, {width(), height()}};
        auto drag = minirender::GizmoDrag::begin(view, *gizmo_target_, gizmo_layout_, hit, pointer);
        if (drag && on_transform_begin && on_transform_begin()) {
            gizmo_drag_  = std::move(drag);
            gizmo_hover_ = hit.handle;
            mark_preview_changed();
            requestUpdate();
        }
    }
    event->accept();
}

void VulkanViewport::mouseMoveEvent(QMouseEvent* event) {
    if (dragging_split_) {
        dragging_split_ = event->buttons().testFlag(Qt::LeftButton);
        if (dragging_split_ && width() > 0) {
            set_split_position(static_cast<float>(event->position().x() / width()));
        }
        event->accept();
        return;
    }
    if (!scene_ || offline_visible_) {
        return;
    }
    const glm::dvec2 pointer(event->position().x(), event->position().y());
    if (gizmo_drag_) {
        if (!event->buttons().testFlag(Qt::LeftButton)) {
            finish_gizmo_drag(true);
            return;
        }
        try {
            const auto value = gizmo_drag_->update(pointer);
            if (!on_transform_preview || !on_transform_preview(value)) {
                finish_gizmo_drag(false);
            }
        } catch (const std::exception& error) {
            finish_gizmo_drag(false);
            if (on_interaction_error) {
                on_interaction_error(error.what());
            }
        }
        event->accept();
        return;
    }
    const auto hovered = gizmo_layout_.hit(pointer).handle;
    if (hovered != gizmo_hover_) {
        gizmo_hover_ = hovered;
        mark_preview_changed();
        requestUpdate();
    }
    // 只响应从本视口开始的拖拽；释放或失焦后不继承外部窗口的按钮状态。
    pressed_buttons_ &= event->buttons();
    if (pressed_buttons_ == Qt::NoButton) {
        last_mouse_ = event->position();
        return;
    }
    const auto delta = event->position() - last_mouse_;
    last_mouse_      = event->position();
    if ((event->position() - mouse_origin_).manhattanLength() > 3.0) {
        dragged_ = true;
    }
    const float dx       = static_cast<float>(delta.x());
    const float dy       = static_cast<float>(delta.y());
    const float distance = glm::length(camera_.target - camera_.position);
    if (pressed_buttons_.testFlag(Qt::MiddleButton) ||
        (pressed_buttons_.testFlag(Qt::LeftButton) &&
         event->modifiers().testFlag(Qt::ShiftModifier))) {
        set_camera(
            minirender::pan_camera(camera_, {-dx * distance * 0.002F, dy * distance * 0.002F}));
    } else if (pressed_buttons_.testFlag(Qt::RightButton)) {
        set_camera(minirender::rotate_camera(camera_, -dx * 0.005F, -dy * 0.005F, false));
    } else if (pressed_buttons_.testFlag(Qt::LeftButton) && dragged_) {
        set_camera(minirender::rotate_camera(camera_, -dx * 0.005F, dy * 0.005F, true));
    }
    event->accept();
}

void VulkanViewport::mouseReleaseEvent(QMouseEvent* event) {
    if (dragging_split_ && event->button() == Qt::LeftButton) {
        dragging_split_ = false;
        event->accept();
        return;
    }
    if (gizmo_drag_ && event->button() == Qt::LeftButton) {
        finish_gizmo_drag(true);
        event->accept();
        return;
    }
    const bool owned_press = pressed_buttons_.testFlag(event->button());
    pressed_buttons_ &= ~Qt::MouseButtons(event->button());
    if (owned_press && scene_ && !offline_visible_ && !point_visible_ && !sdf_visible_ &&
        event->button() == Qt::LeftButton && !dragged_ && width() > 0 && height() > 0) {
        const glm::vec2 uv{static_cast<float>(event->position().x()) / static_cast<float>(width()),
                           static_cast<float>(event->position().y()) /
                               static_cast<float>(height())};
        const auto      selected = minirender::pick_object(
            *scene_,
            camera_.generate_ray(uv, static_cast<float>(width()) / static_cast<float>(height())));
        if (on_object_picked) {
            on_object_picked(selected.value_or(""));
        }
    }
    event->accept();
}

void VulkanViewport::wheelEvent(QWheelEvent* event) {
    finish_gizmo_drag(true);
    if (scene_ && !offline_visible_) {
        set_camera(minirender::dolly_camera(
            camera_, std::exp(-static_cast<float>(event->angleDelta().y()) / 1200.0F)));
    }
    event->accept();
}

void VulkanViewport::keyPressEvent(QKeyEvent* event) {
    if (!scene_ || offline_visible_) {
        return;
    }
    if (event->key() == Qt::Key_Escape && gizmo_drag_) {
        finish_gizmo_drag(false);
        event->accept();
        return;
    }
    if (event->modifiers() == Qt::NoModifier &&
        (event->key() == Qt::Key_V || event->key() == Qt::Key_T || event->key() == Qt::Key_R)) {
        set_gizmo_mode(event->key() == Qt::Key_T   ? minirender::GizmoMode::translate
                       : event->key() == Qt::Key_R ? minirender::GizmoMode::rotate
                                                   : minirender::GizmoMode::select);
        event->accept();
        return;
    }
    glm::vec3 move(0);
    switch (event->key()) {
    case Qt::Key_W:
        move.z = 1;
        break;
    case Qt::Key_S:
        move.z = -1;
        break;
    case Qt::Key_A:
        move.x = -1;
        break;
    case Qt::Key_D:
        move.x = 1;
        break;
    case Qt::Key_Q:
        move.y = -1;
        break;
    case Qt::Key_E:
        move.y = 1;
        break;
    default:
        QWindow::keyPressEvent(event);
        return;
    }
    const float speed = event->modifiers().testFlag(Qt::ShiftModifier) ? 0.3F : 0.1F;
    set_camera(minirender::move_camera(camera_, move * speed));
    event->accept();
}

void VulkanViewport::set_gizmo_mode(minirender::GizmoMode mode) {
    if (mode == gizmo_mode_) {
        return;
    }
    finish_gizmo_drag(true);
    gizmo_mode_  = mode;
    gizmo_hover_ = minirender::GizmoHandle::none;
    rebuild_gizmo();
    mark_preview_changed();
    requestUpdate();
    if (on_gizmo_mode_changed) {
        on_gizmo_mode_changed(mode);
    }
}

minirender::GizmoMode VulkanViewport::gizmo_mode() const noexcept {
    return gizmo_mode_;
}

void VulkanViewport::set_gizmo_target(std::string                          object_id,
                                      std::optional<minirender::Transform> transform) {
    if (object_id != gizmo_target_id_) {
        finish_gizmo_drag(false);
    }
    gizmo_target_id_ = std::move(object_id);
    gizmo_target_    = std::move(transform);
    rebuild_gizmo();
    mark_preview_changed();
    requestUpdate();
}

void VulkanViewport::rebuild_gizmo() {
    gizmo_layout_ = {};
    if (gizmo_target_ && scene_ && !offline_visible_ && !point_visible_ && !sdf_visible_ &&
        width() > 0 && height() > 0) {
        const minirender::GizmoView view{camera_, {width(), height()}};
        gizmo_layout_ = minirender::make_gizmo_layout(view, *gizmo_target_, gizmo_mode_);
    }
}

const minirender::GizmoLayout& VulkanViewport::gizmo_layout() const noexcept {
    return gizmo_layout_;
}

minirender::GizmoHandle VulkanViewport::highlighted_gizmo_handle() const noexcept {
    return gizmo_drag_ ? gizmo_drag_->handle() : gizmo_hover_;
}

bool VulkanViewport::gizmo_dragging() const noexcept {
    return gizmo_drag_.has_value();
}

void VulkanViewport::finish_gizmo_drag(bool commit) {
    if (!gizmo_drag_) {
        return;
    }
    // 先清空手势，再回调文档更新，允许回调重建场景或改变目标。
    gizmo_drag_.reset();
    gizmo_hover_     = minirender::GizmoHandle::none;
    pressed_buttons_ = Qt::NoButton;
    dragged_         = true;
    if (on_transform_end) {
        on_transform_end(commit);
    }
    rebuild_gizmo();
    mark_preview_changed();
    requestUpdate();
}
