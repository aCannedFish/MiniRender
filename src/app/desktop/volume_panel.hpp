#pragma once

#include <minirender/scene/medium.hpp>

#include <QWidget>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;

// 区域编辑使用独立草稿，完整候选通过校验后再提交一次场景历史。
// 刷新按 media 值与文档代数判断；无关编辑保留草稿及未显示的 double 有效位。
class VolumePanel final : public QWidget {
  public:
    explicit VolumePanel(QWidget* parent = nullptr);
    void set_document(std::span<const minirender::MediumRegion> regions, std::uint64_t generation);
    void set_asset_busy(bool busy);
    std::function<void(std::vector<minirender::MediumRegion>)> on_changed;
    std::function<void(const QString&)> on_demo;
    std::function<void()> on_render;
    std::function<bool()> render_active;

  private:
    using VectorAccess = std::function<glm::dvec3&(minirender::MediumRegion&)>;
    QWidget* vector_field(const char* name, std::array<QDoubleSpinBox*, 3>& controls,
                          VectorAccess access, double low, double high, bool rgb);
    void select();
    void refresh_fields();
    void update_buttons();
    void edit(const std::function<void(minirender::MediumRegion&)>& operation);
    void invoke(const std::function<void()>& operation);
    void apply();
    void add();
    void remove();
    void publish(std::vector<minirender::MediumRegion> regions, std::string selected);
    std::vector<minirender::MediumRegion> source_;
    std::optional<minirender::MediumRegion> draft_;
    std::string selected_;
    std::uint64_t generation_ = 0;
    bool initialized_ = false, refreshing_ = false, dirty_ = false, busy_ = false;
    QComboBox *regions_ = nullptr, *demo_ = nullptr;
    QLineEdit* name_ = nullptr;
    QCheckBox* enabled_ = nullptr;
    std::array<QDoubleSpinBox*, 3> minimum_{}, maximum_{}, absorption_{}, scattering_{};
    QDoubleSpinBox* g_ = nullptr;
    QWidget* parameters_ = nullptr;
    QLabel *notice_ = nullptr, *summary_ = nullptr;
    QPushButton *add_ = nullptr, *remove_ = nullptr, *apply_ = nullptr;
    QPushButton *load_ = nullptr, *render_ = nullptr;
};
