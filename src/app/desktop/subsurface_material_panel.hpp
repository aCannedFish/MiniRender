#pragma once

#include <minirender/scene/scene.hpp>

#include <QWidget>
#include <array>
#include <functional>
#include <optional>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;

// 光学参数使用完整 double 草稿，整组校验后提交一次历史；无关场景编辑保留草稿。
class SubsurfaceMaterialPanel final : public QWidget {
  public:
    explicit SubsurfaceMaterialPanel(QWidget* parent = nullptr);
    void set_material(const minirender::Material* material, std::uint64_t generation);
    std::function<void(std::shared_ptr<const minirender::SubsurfaceParameters>,
                       std::optional<double>)>
        on_changed;
    std::function<void(bool start)> on_render;
    std::function<bool()> render_active;

  private:
    QWidget* vector_field(const char* name, std::array<QDoubleSpinBox*, 3>& fields,
                          bool absorption);
    void refresh_fields();
    void update_buttons();
    void apply();
    bool dirty() const;
    std::optional<minirender::SubsurfaceParameters> source_, draft_;
    std::string material_id_;
    std::uint64_t generation_ = 0;
    std::array<QDoubleSpinBox*, 3> absorption_{}, scattering_{};
    QDoubleSpinBox* density_;
    QComboBox* preset_;
    QPushButton *apply_, *restore_, *render_;
    QLabel* notice_;
    QString error_;
    bool viewing_task_ = false;
};
