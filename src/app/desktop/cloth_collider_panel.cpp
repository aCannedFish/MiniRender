#include "cloth_collider_panel.hpp"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLocale>

ClothColliderPanel::ClothColliderPanel(QWidget* parent) : QGroupBox("布料碰撞", parent) {
    setObjectName("cloth_collider_panel");
    auto* form = new QFormLayout(this);
    form->setVerticalSpacing(12);
    kind_ = new QLabel(this);
    kind_->setWordWrap(true);
    enabled_ = new QCheckBox("参与布料碰撞", this);
    enabled_->setObjectName("cloth_collider_enabled");
    friction_ = new QDoubleSpinBox(this);
    friction_->setObjectName("cloth_collider_friction");
    friction_->setRange(0, 1);
    friction_->setDecimals(5);
    friction_->setSingleStep(.05);
    friction_->setKeyboardTracking(false);
    friction_->setLocale(QLocale::c());
    form->addRow(kind_);
    form->addRow(enabled_);
    form->addRow("摩擦系数", friction_);
    connect(enabled_, &QCheckBox::toggled, this, [this](bool enabled) {
        if (!refreshing_ && current_ && on_changed) {
            auto value = *current_;
            value.enabled = enabled;
            on_changed(value);
        }
    });
    connect(friction_, &QDoubleSpinBox::valueChanged, this, [this](double friction) {
        if (!refreshing_ && current_ && on_changed) {
            auto value = *current_;
            value.friction = friction;
            on_changed(value);
        }
    });
    set_collider(std::nullopt);
}

void ClothColliderPanel::set_collider(std::optional<minirender::ClothCollider> collider) {
    refreshing_ = true;
    current_ = std::move(collider);
    setVisible(current_.has_value());
    if (current_) {
        const bool sphere = current_->kind == minirender::ClothColliderKind::sphere;
        kind_->setText(sphere ? "球形，随物体移动；缩放需三轴等比。"
                              : "平面，随物体移动和旋转。法线朝向为可接触一侧。");
        enabled_->setChecked(current_->enabled);
        friction_->setValue(current_->friction);
    }
    refreshing_ = false;
}
