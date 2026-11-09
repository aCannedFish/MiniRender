#pragma once

#include "geometry_controller.hpp"
#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QFormLayout;

struct GeometryParameters {
    minirender::LoopSettings loop;
    std::optional<minirender::DisplacementSettings> displacement;
    std::string height_id;
    std::optional<minirender::SimplifySettings> simplify;
    std::optional<minirender::RemeshSettings> remesh;
};

// 几何参数只在点击生成时提交，修改控件只更新基于数组规模的预算估计。
// 不在 UI 中建立半边或扫描三角形；任务进度从 controller 的固定阶段计数读取。
class GeometryPanel final : public QWidget {
  public:
    GeometryPanel(GeometryController& controller, QWidget* parent = nullptr);
    GeometryParameters parameters() const;
    void set_context(const minirender::SceneDocument& document, const std::string& selected_id);
    void set_asset_task_busy(bool busy);
    void select_height(const std::string& id);
    void show_result(const GeometryTaskResult& result);
    std::function<void()> on_apply;
    std::function<void()> on_import;
    std::function<void()> on_export;

  private:
    void update();
    GeometryController& controller_;
    std::shared_ptr<const minirender::MeshData> mesh_;
    bool external_busy_ = false;
    QFormLayout* form_;
    QWidget* height_row_;
    QWidget* constraints_;
    QWidget* remeshing_;
    QFormLayout* tool_form_;
    QDoubleSpinBox* remesh_length_;
    QDoubleSpinBox* remesh_step_;
    QSpinBox* remesh_iterations_;
    QSpinBox* remesh_budget_;
    QCheckBox* remesh_projection_;
    QComboBox* operation_;
    QSpinBox* target_faces_;
    QCheckBox *error_limit_, *boundary_, *uv_seams_, *hard_normals_, *creases_;
    QDoubleSpinBox *quadric_error_, *crease_angle_;
    QLabel* note_;
    QSpinBox *levels_, *budget_;
    QCheckBox* displace_;
    QComboBox* height_;
    QDoubleSpinBox *scale_, *midlevel_, *tolerance_;
    QPushButton *apply_, *cancel_, *import_, *export_;
    QLabel *target_, *estimate_, *stage_, *counts_, *result_, *metrics_;
    QProgressBar* progress_;
};
