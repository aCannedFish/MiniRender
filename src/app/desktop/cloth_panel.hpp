#pragma once

#include "cloth_controller.hpp"
#include <QWidget>

class QComboBox;
class QSpinBox;
class QDoubleSpinBox;
class QPushButton;
class QLabel;
class QProgressBar;

// 播放与物理参数页。草稿与场景组件分开，应用参数后由后台生成完整状态再进入历史。
class ClothPanel final : public QWidget {
  public:
    ClothPanel(ClothController& controller, QWidget* parent = nullptr);
    void set_document(const minirender::SceneDocument& scene, const std::string& target);
    std::function<void(int)> on_demo;
    std::function<void(const std::string&)> on_target;
    std::function<void(std::shared_ptr<const minirender::ClothDefinition>, bool)> on_apply;
    std::function<void()> on_freeze;
    std::function<void()> on_preview;

  private:
    void apply();
    void poll();
    void refresh_parameters();
    ClothController& controller_;
    std::shared_ptr<const minirender::ClothDefinition> source_;
    minirender::ClothDefinition draft_;
    bool refreshing_ = false, geometry_dirty_ = false;
    QComboBox *target_ = nullptr, *demo_ = nullptr, *pins_ = nullptr;
    QSpinBox *columns_ = nullptr, *rows_ = nullptr, *substeps_ = nullptr;
    QDoubleSpinBox *width_ = nullptr, *height_ = nullptr, *density_ = nullptr;
    QDoubleSpinBox *structural_ = nullptr, *shear_ = nullptr, *bending_ = nullptr;
    QDoubleSpinBox *spring_damping_ = nullptr, *velocity_damping_ = nullptr, *thickness_ = nullptr;
    std::array<QDoubleSpinBox*, 3> gravity_{}, wind_{};
    QDoubleSpinBox *air_ = nullptr, *drag_ = nullptr;
    QPushButton *play_ = nullptr, *pause_ = nullptr, *step_ = nullptr, *reset_ = nullptr,
                *freeze_ = nullptr, *apply_ = nullptr;
    QLabel *status_ = nullptr, *details_ = nullptr, *notice_ = nullptr;
    QProgressBar* progress_ = nullptr;
};
