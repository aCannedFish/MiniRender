#pragma once

#include <minirender/scene/cloth_collider.hpp>

#include <QGroupBox>
#include <functional>
#include <optional>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;

// 编辑场景物体已有的碰撞形状。形状使用物体变换，开关和摩擦独立于渲染材质。
class ClothColliderPanel final : public QGroupBox {
  public:
    explicit ClothColliderPanel(QWidget* parent = nullptr);
    void set_collider(std::optional<minirender::ClothCollider> collider);
    std::function<void(const minirender::ClothCollider&)> on_changed;

  private:
    std::optional<minirender::ClothCollider> current_;
    QCheckBox* enabled_ = nullptr;
    QDoubleSpinBox* friction_ = nullptr;
    QLabel* kind_ = nullptr;
    bool refreshing_ = false;
};
