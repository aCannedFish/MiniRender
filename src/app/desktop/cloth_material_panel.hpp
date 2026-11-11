#pragma once

#include <QWidget>
#include <minirender/scene/scene.hpp>

#include <functional>
#include <optional>

class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QLineEdit;
class QLabel;
class QFormLayout;

// 材质页中的布料参数。每次只修改实际编辑的字段；曲线两列通过按钮一起提交。
// 参数副本验证成功后通知 Workbench，由主场景历史完成事务和快照发布。
class ClothMaterialPanel final : public QWidget {
  public:
    explicit ClothMaterialPanel(QWidget* parent = nullptr);
    void set_material(const minirender::Material* material, std::uint64_t scene_generation);
    std::function<void(std::shared_ptr<const minirender::ClothAppearance>, std::optional<double>)>
        on_changed;

  private:
    struct ThreadControls {
        std::array<QDoubleSpinBox*, 3> color{};
        QDoubleSpinBox *kd = nullptr, *surface = nullptr, *volume = nullptr;
        QLineEdit *angles = nullptr, *lengths = nullptr;
    };
    void build_thread(std::size_t index, QFormLayout* layout);
    void edit(const std::function<void(minirender::ClothAppearanceSettings&)>& work);
    void apply_curve(std::size_t index);
    void refresh();
    std::shared_ptr<const minirender::ClothAppearance> appearance_;
    std::string material_id_;
    std::uint64_t generation_ = 0;
    bool refreshing_ = false;
    QComboBox* preset_ = nullptr;
    QSpinBox* samples_ = nullptr;
    QDoubleSpinBox *rotation_ = nullptr, *coverage_ = nullptr, *ratio_ = nullptr,
                   *correlation_ = nullptr;
    std::array<ThreadControls, 2> threads_;
    QLabel* notice_ = nullptr;
};
