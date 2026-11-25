// 布料模拟参数控件、状态刷新与用户操作回调。

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
    // 建立播放与物理参数控件，从控制器读取当前模拟状态。
    ClothPanel(ClothController& controller, QWidget* parent = nullptr);
    // 从场景和目标物体恢复布料定义并刷新编辑草稿。
    void set_document(const minirender::SceneDocument& scene, const std::string& target);
    // 由工作台加载场景、选择目标、提交参数或冻结当前模拟帧。
    std::function<void(int)>                                                      on_demo;
    std::function<void(const std::string&)>                                       on_target;
    std::function<void(std::shared_ptr<const minirender::ClothDefinition>, bool)> on_apply;
    std::function<void()>                                                         on_freeze;
    std::function<void()>                                                         on_preview;

  private:
    // 读取并校验物理参数，通过回调提交布料定义及网格重建标志。
    void apply();
    // 从模拟控制器刷新帧号、运行状态和按钮。
    void poll();
    // 用草稿填充参数控件，刷新期间阻止信号回写。
    void refresh_parameters();
    ClothController& controller_;
    // source_ 持有已发布的只读定义，draft_ 是面板正在编辑的副本。
    std::shared_ptr<const minirender::ClothDefinition> source_;
    minirender::ClothDefinition                        draft_;
    bool                                               refreshing_     = false;
    bool                                               geometry_dirty_ = false;
    QComboBox *     target_ = nullptr, *demo_ = nullptr, *pins_ = nullptr;
    QSpinBox *      columns_ = nullptr, *rows_ = nullptr, *substeps_ = nullptr;
    QDoubleSpinBox *width_ = nullptr, *height_ = nullptr, *density_ = nullptr;
    QDoubleSpinBox *structural_ = nullptr, *shear_ = nullptr, *bending_ = nullptr;
    QDoubleSpinBox *spring_damping_ = nullptr, *velocity_damping_ = nullptr, *thickness_ = nullptr;
    std::array<QDoubleSpinBox*, 3> gravity_{}, wind_{};
    QDoubleSpinBox *               air_ = nullptr, *drag_ = nullptr;
    QPushButton *play_ = nullptr, *pause_ = nullptr, *step_ = nullptr, *reset_ = nullptr,
                *freeze_ = nullptr, *apply_ = nullptr;
    QLabel *      status_ = nullptr, *details_ = nullptr, *notice_ = nullptr;
    QProgressBar* progress_ = nullptr;
};
