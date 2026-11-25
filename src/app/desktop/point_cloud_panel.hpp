// 点云参数控件、状态刷新与用户操作回调。

#pragma once

#include "geometry_controller.hpp"

#include <QWidget>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;

// 工具输入保持不可变；采样来源还记录物体及原网格，用于完成时核对替换目标。
struct PointCloudSource {
    std::shared_ptr<const minirender::PointCloud> cloud;
    std::filesystem::path                         file;
    std::string                                   name;
    std::string                                   object_id;
    std::shared_ptr<const minirender::MeshData>   sampled_mesh;
};

// 点云工作页拥有输入、已完成法线及统计，场景修改通过 Workbench 回调执行。
// 参数编辑仅更新按钮与说明；耗时操作由 GeometryController 的 worker 执行。
class PointCloudPanel final : public QWidget {
  public:
    // 绑定后台几何任务的状态，建立采样、法线和重建控件。
    explicit PointCloudPanel(GeometryController& controller, QWidget* parent = nullptr);
    // 借用当前的点云输入。source_revision 在清除或更换输入后递增。
    const PointCloudSource& source() const noexcept;
    std::uint64_t source_revision() const noexcept;
    // 优先返回采样来源物体的 ID，从文件导入时返回已生成的目标物体 ID。
    const std::string& target_id() const noexcept;
    // 替换点云和预览，清除上一输入对应的法线、重建结果和目标记录。
    void set_source(PointCloudSource                                    source,
                    std::shared_ptr<const minirender::PointPreviewData> preview);
    // 释放本页的输入和派生结果，并更新可用操作。
    void clear_source();
    // 根据场景当前选择更新采样按钮和来源提示。
    void set_selected_mesh(bool present, const QString& name);
    // 记录重建目标是否仍有效，以及结果将新建还是替换网格。
    void set_target_state(bool valid, bool create);
    // 导入或导出占用资产任务时，更新按钮的可用状态。
    void set_asset_task_busy(bool busy);
    // 同步视口中的点云开关，阻止控件回写重复触发预览。
    void set_preview_visible(bool visible);
    // 校验结果属于当前输入，保存法线缓存并更新预览和建议球半径。
    void show_normals(std::shared_ptr<const PointNormalCache>             cache,
                      std::shared_ptr<const minirender::PointPreviewData> preview);
    // 接收滚球重建结果，同时更新法线、预览和拓扑统计。
    void show_reconstruction(const GeometryTaskResult& result);
    // 用弱引用记录本页生成的网格，供撤销、重做后核对目标。
    void remember_generated(const std::shared_ptr<const minirender::MeshData>& mesh,
                            const std::string&                                 object_id);
    // 判断目标网格是否仍为原采样网格或同一来源的重建结果。
    bool allows_mesh(const std::shared_ptr<const minirender::MeshData>& mesh) const;
    // 法线准备完成时返回带法线的点云，否则返回原始输入。
    std::shared_ptr<const minirender::PointCloud> display_cloud() const;
    // 返回已为视口准备的点和线数据，调用者共享其只读所有权。
    std::shared_ptr<const minirender::PointPreviewData> preview_data() const noexcept;
    // 读取表面采样的点数和随机种子，可用相同设置复现同一批点。
    std::size_t sample_count() const;
    std::uint64_t sample_seed() const;
    // 读取是否将新建网格等比缩放、居中并放到地面上。
    bool fit_new_object() const;
    // 读取并校验邻域大小、距离上限和法线定向设置。
    minirender::PointNormalSettings normal_parameters() const;
    // 为后台重建固定本次输入、法线设置及滚球半径和预算。
    PointReconstructionInput reconstruction_parameters() const;
    // 收集点尺寸、法线和边界可视化选项。
    minirender::PointPreviewSettings display_parameters() const;

    // 工作台处理文件对话框、场景写入和后台任务，面板通过这些回调传递操作。
    std::function<void()>                                 on_import;
    std::function<void()>                                 on_sample;
    std::function<void()>                                 on_prepare;
    std::function<void()>                                 on_reconstruct;
    std::function<void()>                                 on_export;
    std::function<void()>                                 on_locate;
    std::function<void(bool)>                             on_preview;
    std::function<void(minirender::PointPreviewSettings)> on_display;

  private:
    // 根据点云来源、任务状态和目标有效性更新按钮与进度。
    void update_state();
    // 显示点云规模、法线与最近一次重建的统计。
    void update_metrics();
    // 在自动半径模式中，根据法线预处理的邻域间距更新球半径。
    void update_suggested_radii();
    // 控制器管理任务。面板持有当前输入、法线缓存和已发布的预览结果。
    GeometryController&                                    controller_;
    PointCloudSource                                       source_;
    std::shared_ptr<const PointNormalCache>                normals_;
    std::shared_ptr<const minirender::BallPivotResult>     result_;
    std::shared_ptr<const minirender::PointPreviewData>    preview_;
    std::vector<std::weak_ptr<const minirender::MeshData>> generated_;
    std::uint64_t                                          revision_      = 0;
    bool                                                   selected_mesh_ = false;
    bool                                                   target_valid_  = true;
    bool                                                   will_create_   = true;
    std::string                                            output_object_id_;
    bool                                                   external_busy_ = false;
    // 以下控件归本面板的 Qt 对象树所有，分别显示任务状态、采样参数和操作。
    QLabel*         source_label_;
    QLabel*         warning_;
    QLabel*         stage_;
    QLabel*         counts_;
    QLabel*         metrics_;
    QProgressBar*   progress_;
    QSpinBox*       samples_;
    QSpinBox*       seed_;
    QSpinBox*       neighbors_;
    QSpinBox*       faces_;
    QSpinBox*       work_;
    QDoubleSpinBox* distance_;
    QDoubleSpinBox* point_size_;
    QDoubleSpinBox* normal_length_;
    QLineEdit*      radii_;
    QCheckBox*      estimate_;
    QCheckBox*      orient_;
    QCheckBox*      flip_;
    QCheckBox*      automatic_;
    QCheckBox*      fit_;
    QCheckBox*      visible_;
    QCheckBox*      normal_colors_;
    QCheckBox*      show_normals_;
    QCheckBox*      show_boundary_;
    QPushButton*    import_;
    QPushButton*    sample_;
    QPushButton*    prepare_;
    QPushButton*    reconstruct_;
    QPushButton*    export_;
    QPushButton*    cancel_;
    QPushButton*    locate_;
};
