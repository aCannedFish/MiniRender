// 几何处理参数控件、状态刷新与用户操作回调。

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

// 从面板读取的一组几何参数，后台作业获得其副本。
struct GeometryParameters {
    minirender::LoopSettings                        loop;
    std::optional<minirender::DisplacementSettings> displacement;
    std::string                                     height_id;
    std::optional<minirender::SimplifySettings>     simplify;
    std::optional<minirender::RemeshSettings>       remesh;
    std::optional<minirender::AdaptiveSettings>     adaptive;
};

// 几何参数只在点击生成时提交，修改控件只更新基于数组规模的预算估计。
// 不在 UI 中建立半边或扫描三角形；任务进度从 controller 的固定阶段计数读取。
class GeometryPanel final : public QWidget {
  public:
    // 绑定几何任务控制器，并建立共用预算与各算法的参数控件。
    GeometryPanel(GeometryController& controller, QWidget* parent = nullptr);
    // 读取当前算法及参数，生成独立的后台任务输入。
    GeometryParameters parameters() const;
    // 同步当前目标网格、可用高度纹理及基于网格规模的预算估计。
    void set_context(const minirender::SceneDocument& document, const std::string& selected_id);
    // 在其他资产操作期间更新生成、导入和导出控件。
    void set_asset_task_busy(bool busy);
    // 按场景纹理 ID 选中对应高度图。
    void select_height(const std::string& id);
    // 显示最近一次作业的拓扑规模和误差统计。
    void show_result(const GeometryTaskResult& result);
    // 由工作台启动几何作业、导入高度资源或导出生成的网格。
    std::function<void()> on_apply;
    std::function<void()> on_import;
    std::function<void()> on_export;

  private:
    // 按所选算法切换参数组，并根据任务状态刷新估计和进度。
    void update();
    GeometryController&                         controller_;
    std::shared_ptr<const minirender::MeshData> mesh_;
    bool                                        external_busy_ = false;
    QFormLayout*                                form_;
    QWidget*                                    height_row_;
    QWidget*                                    constraints_;
    QWidget*                                    remeshing_;
    QFormLayout*                                tool_form_;
    QWidget*                                    adaptive_group_;
    QSpinBox*                                   adaptive_minimum_;
    QSpinBox*                                   adaptive_maximum_;
    QSpinBox*                                   adaptive_budget_;
    QComboBox*                                  adaptive_metric_;
    QDoubleSpinBox*                             adaptive_error_;
    QDoubleSpinBox*                             remesh_length_;
    QDoubleSpinBox*                             remesh_step_;
    QSpinBox*                                   remesh_iterations_;
    QSpinBox*                                   remesh_budget_;
    QCheckBox*                                  remesh_projection_;
    QComboBox*                                  operation_;
    QSpinBox*                                   target_faces_;
    QCheckBox *     error_limit_, *boundary_, *uv_seams_, *hard_normals_, *creases_;
    QDoubleSpinBox *quadric_error_, *crease_angle_;
    QLabel*         note_;
    QSpinBox *      levels_, *budget_;
    QCheckBox*      displace_;
    QComboBox*      height_;
    QDoubleSpinBox *scale_, *midlevel_, *tolerance_;
    QPushButton *   apply_, *cancel_, *import_, *export_;
    QLabel *        target_, *estimate_, *stage_, *counts_, *result_, *metrics_;
    QProgressBar*   progress_;
};
