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
    std::filesystem::path file;
    std::string name;
    std::string object_id;
    std::shared_ptr<const minirender::MeshData> sampled_mesh;
};

// 点云工作页拥有输入、已完成法线及统计，场景修改通过 Workbench 回调执行。
// 参数编辑仅更新按钮与说明；耗时操作由 GeometryController 的 worker 执行。
class PointCloudPanel final : public QWidget {
  public:
    explicit PointCloudPanel(GeometryController& controller, QWidget* parent = nullptr);
    const PointCloudSource& source() const noexcept;
    std::uint64_t source_revision() const noexcept;
    const std::string& target_id() const noexcept;
    void set_source(PointCloudSource source,
                    std::shared_ptr<const minirender::PointPreviewData> preview);
    void clear_source();
    void set_selected_mesh(bool present, const QString& name);
    void set_target_state(bool valid, bool create);
    void set_asset_task_busy(bool busy);
    void set_preview_visible(bool visible);
    void show_normals(std::shared_ptr<const PointNormalCache> cache,
                      std::shared_ptr<const minirender::PointPreviewData> preview);
    void show_reconstruction(const GeometryTaskResult& result);
    void remember_generated(const std::shared_ptr<const minirender::MeshData>& mesh,
                            const std::string& object_id);
    bool allows_mesh(const std::shared_ptr<const minirender::MeshData>& mesh) const;
    std::shared_ptr<const minirender::PointCloud> display_cloud() const;
    std::shared_ptr<const minirender::PointPreviewData> preview_data() const noexcept;
    std::size_t sample_count() const;
    std::uint64_t sample_seed() const;
    bool fit_new_object() const;
    minirender::PointNormalSettings normal_parameters() const;
    PointReconstructionInput reconstruction_parameters() const;
    minirender::PointPreviewSettings display_parameters() const;

    std::function<void()> on_import;
    std::function<void()> on_sample;
    std::function<void()> on_prepare;
    std::function<void()> on_reconstruct;
    std::function<void()> on_export;
    std::function<void()> on_locate;
    std::function<void(bool)> on_preview;
    std::function<void(minirender::PointPreviewSettings)> on_display;

  private:
    void update_state();
    void update_metrics();
    void update_suggested_radii();
    GeometryController& controller_;
    PointCloudSource source_;
    std::shared_ptr<const PointNormalCache> normals_;
    std::shared_ptr<const minirender::BallPivotResult> result_;
    std::shared_ptr<const minirender::PointPreviewData> preview_;
    std::vector<std::weak_ptr<const minirender::MeshData>> generated_;
    std::uint64_t revision_ = 0;
    bool selected_mesh_ = false;
    bool target_valid_ = true;
    bool will_create_ = true;
    std::string output_object_id_;
    bool external_busy_ = false;
    QLabel* source_label_;
    QLabel* warning_;
    QLabel* stage_;
    QLabel* counts_;
    QLabel* metrics_;
    QProgressBar* progress_;
    QSpinBox* samples_;
    QSpinBox* seed_;
    QSpinBox* neighbors_;
    QSpinBox* faces_;
    QSpinBox* work_;
    QDoubleSpinBox* distance_;
    QDoubleSpinBox* point_size_;
    QDoubleSpinBox* normal_length_;
    QLineEdit* radii_;
    QCheckBox* estimate_;
    QCheckBox* orient_;
    QCheckBox* flip_;
    QCheckBox* automatic_;
    QCheckBox* fit_;
    QCheckBox* visible_;
    QCheckBox* normal_colors_;
    QCheckBox* show_normals_;
    QCheckBox* show_boundary_;
    QPushButton* import_;
    QPushButton* sample_;
    QPushButton* prepare_;
    QPushButton* reconstruct_;
    QPushButton* export_;
    QPushButton* cancel_;
    QPushButton* locate_;
};
