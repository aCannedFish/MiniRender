#include "point_cloud_panel.hpp"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace {
QLabel* label(QWidget* parent, const char* name) {
    auto* value = new QLabel(parent);
    value->setObjectName(name);
    value->setTextFormat(Qt::PlainText);
    value->setWordWrap(true);
    value->setMinimumWidth(0);
    return value;
}
QSpinBox* integer(QWidget* parent, const char* name, int minimum, int maximum, int initial) {
    auto* value = new QSpinBox(parent);
    value->setObjectName(name);
    value->setRange(minimum, maximum);
    value->setValue(initial);
    value->setKeyboardTracking(false);
    return value;
}
QDoubleSpinBox* scalar(QWidget* parent, const char* name, double minimum, double maximum,
                       double initial, int decimals) {
    auto* value = new QDoubleSpinBox(parent);
    value->setObjectName(name);
    value->setRange(minimum, maximum);
    value->setDecimals(decimals);
    value->setValue(initial);
    value->setKeyboardTracking(false);
    return value;
}
} // namespace

PointCloudPanel::PointCloudPanel(GeometryController& controller, QWidget* parent)
    : QWidget(parent), controller_(controller) {
    setObjectName("point_cloud_panel");
    auto* form = new QFormLayout(this);
    form->setContentsMargins(10, 12, 10, 12);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setVerticalSpacing(12);
    source_label_ = label(this, "points_source");
    form->addRow(source_label_);
    const auto button = [&](const char* name, const char* text) {
        auto* value = new QPushButton(QString::fromUtf8(text), this);
        value->setObjectName(name);
        return value;
    };
    import_ = button("points_import", "导入 PLY 点云");
    sample_ = button("points_sample", "从选中网格采样");
    form->addRow(import_);
    samples_ = integer(this, "points_samples", 100, 500000, 5000);
    seed_ = integer(this, "points_seed", 0, std::numeric_limits<int>::max(), 101);
    form->addRow("采样点数", samples_);
    form->addRow("采样种子", seed_);
    form->addRow(sample_);
    estimate_ = new QCheckBox("估计法线", this);
    estimate_->setObjectName("points_estimate");
    estimate_->setChecked(true);
    estimate_->setToolTip("关闭后使用导入文件或表面采样提供的原始法线。");
    orient_ = new QCheckBox("统一法线方向", this);
    orient_->setObjectName("points_orient");
    orient_->setChecked(true);
    flip_ = new QCheckBox("整体翻转法线", this);
    flip_->setObjectName("points_flip");
    neighbors_ = integer(this, "points_neighbors", 3, 128, 20);
    distance_ = scalar(this, "points_neighbor_distance", 0, 1000000, 0, 6);
    distance_->setSpecialValueText("不限");
    distance_->setSuffix(" m");
    form->addRow(estimate_);
    form->addRow("邻域点数", neighbors_);
    form->addRow("邻域距离上限", distance_);
    form->addRow(orient_);
    form->addRow(flip_);
    prepare_ = button("points_prepare", "准备法线与间距");
    form->addRow(prepare_);
    automatic_ = new QCheckBox("按点间距建议半径", this);
    automatic_->setObjectName("points_auto_radii");
    automatic_->setChecked(true);
    radii_ = new QLineEdit(this);
    radii_->setObjectName("points_radii");
    radii_->setMaxLength(1024);
    radii_->setPlaceholderText("例如 0.002, 0.003, 0.004");
    radii_->setToolTip("局部米制半径，按递增顺序填写，用逗号或空格分隔。自动建议采用最近邻间距 P90 "
                       "的 2、3、4 倍。");
    faces_ = integer(this, "points_face_budget", 1, 1000000, 500000);
    work_ = integer(this, "points_work_budget", 1000, 100000000, 20000000);
    work_->setSingleStep(1000000);
    fit_ = new QCheckBox("文件导入结果适应场景尺寸", this);
    fit_->setObjectName("points_fit_scene");
    fit_->setChecked(true);
    fit_->setToolTip("新物体通过变换缩放到约两米并放在地面上方，局部网格保留输入单位。来源物体采样"
                     "始终保留原变换。");
    form->addRow(automatic_);
    form->addRow("滚球半径 / m", radii_);
    form->addRow("输出面数上限", faces_);
    form->addRow("候选检查上限", work_);
    form->addRow(fit_);
    warning_ = label(this, "points_warning");
    form->addRow(warning_);
    reconstruct_ = button("points_reconstruct", "重建并应用到场景");
    export_ = button("points_export", "导出当前点云 PLY");
    cancel_ = button("points_cancel", "取消后台任务");
    form->addRow(reconstruct_);
    form->addRow(export_);
    form->addRow(cancel_);
    visible_ = new QCheckBox("点云预览（独立视角）", this);
    visible_->setObjectName("points_visible");
    locate_ = button("points_locate", "定位点云");
    point_size_ = scalar(this, "points_size", 1, 32, 6, 1);
    normal_length_ = scalar(this, "points_normal_length", 0, 100, 3, 1);
    normal_colors_ = new QCheckBox("法线颜色", this);
    normal_colors_->setObjectName("points_normal_colors");
    show_normals_ = new QCheckBox("显示抽样法线", this);
    show_normals_->setObjectName("points_show_normals");
    show_boundary_ = new QCheckBox("显示重建边界", this);
    show_boundary_->setObjectName("points_show_boundary");
    show_boundary_->setChecked(true);
    form->addRow(visible_);
    form->addRow(locate_);
    form->addRow("点大小 / px", point_size_);
    form->addRow(normal_colors_);
    form->addRow(show_normals_);
    form->addRow("法线长度 / 模型 %", normal_length_);
    form->addRow(show_boundary_);
    stage_ = label(this, "points_stage");
    counts_ = label(this, "points_counts");
    progress_ = new QProgressBar(this);
    progress_->setObjectName("points_progress");
    metrics_ = label(this, "points_metrics");
    form->addRow(stage_);
    form->addRow(progress_);
    form->addRow(counts_);
    form->addRow(metrics_);
    const auto action = [this](QPushButton* control, std::function<void()>* callback) {
        connect(control, &QPushButton::clicked, this, [callback] {
            if (*callback) {
                (*callback)();
            }
        });
    };
    action(import_, &on_import);
    action(sample_, &on_sample);
    action(prepare_, &on_prepare);
    action(reconstruct_, &on_reconstruct);
    action(export_, &on_export);
    action(locate_, &on_locate);
    connect(cancel_, &QPushButton::clicked, this, [this] {
        controller_.cancel();
        update_state();
    });
    connect(visible_, &QCheckBox::toggled, this, [this](bool value) {
        if (on_preview) {
            on_preview(value);
        }
    });
    for (auto* value : {estimate_, orient_, flip_, automatic_}) {
        connect(value, &QCheckBox::toggled, this, [this] { update_state(); });
    }
    connect(neighbors_, &QSpinBox::valueChanged, this, [this] { update_state(); });
    connect(distance_, &QDoubleSpinBox::valueChanged, this, [this] { update_state(); });
    connect(radii_, &QLineEdit::textChanged, this, [this] { update_state(); });
    const auto display = [this] {
        if (on_display) {
            on_display(display_parameters());
        }
    };
    for (auto* value : {normal_colors_, show_normals_, show_boundary_}) {
        connect(value, &QCheckBox::toggled, this, display);
    }
    connect(point_size_, &QDoubleSpinBox::valueChanged, this, display);
    connect(normal_length_, &QDoubleSpinBox::valueChanged, this, display);
    auto* timer = new QTimer(this);
    timer->setInterval(100);
    connect(timer, &QTimer::timeout, this, [this] { update_state(); });
    timer->start();
    update_state();
}

const PointCloudSource& PointCloudPanel::source() const noexcept {
    return source_;
}
std::uint64_t PointCloudPanel::source_revision() const noexcept {
    return revision_;
}
const std::string& PointCloudPanel::target_id() const noexcept {
    return source_.object_id.empty() ? output_object_id_ : source_.object_id;
}
std::shared_ptr<const minirender::PointPreviewData> PointCloudPanel::preview_data() const noexcept {
    return preview_;
}
std::size_t PointCloudPanel::sample_count() const {
    return static_cast<std::size_t>(samples_->value());
}
std::uint64_t PointCloudPanel::sample_seed() const {
    return static_cast<std::uint64_t>(seed_->value());
}
bool PointCloudPanel::fit_new_object() const {
    return fit_->isChecked();
}

void PointCloudPanel::set_source(PointCloudSource source,
                                 std::shared_ptr<const minirender::PointPreviewData> preview) {
    source_ = std::move(source);
    preview_ = std::move(preview);
    normals_.reset();
    result_.reset();
    generated_.clear();
    target_valid_ = true;
    will_create_ = source_.object_id.empty();
    output_object_id_.clear();
    ++revision_;
    if (automatic_->isChecked()) {
        radii_->clear();
    }
    update_metrics();
    update_state();
}
void PointCloudPanel::clear_source() {
    set_source({}, {});
}
void PointCloudPanel::set_selected_mesh(bool present, const QString& name) {
    selected_mesh_ = present;
    sample_->setToolTip(present ? QString("当前物体：%1").arg(name) : "请先选择场景网格物体。");
    update_state();
}
void PointCloudPanel::set_target_state(bool valid, bool create) {
    target_valid_ = valid;
    will_create_ = create;
    update_state();
}
void PointCloudPanel::set_asset_task_busy(bool busy) {
    external_busy_ = busy;
    update_state();
}
void PointCloudPanel::set_preview_visible(bool visible) {
    const QSignalBlocker block(visible_);
    visible_->setChecked(visible);
}
void PointCloudPanel::show_normals(std::shared_ptr<const PointNormalCache> cache,
                                   std::shared_ptr<const minirender::PointPreviewData> preview) {
    if (!cache || cache->source != source_.cloud) {
        throw std::logic_error("Point normal result belongs to another source");
    }
    normals_ = std::move(cache);
    preview_ = std::move(preview);
    result_.reset();
    update_suggested_radii();
    update_metrics();
    update_state();
}
void PointCloudPanel::show_reconstruction(const GeometryTaskResult& result) {
    show_normals(result.point_normals, result.point_preview);
    result_ = result.point_reconstruction;
    update_metrics();
    update_state();
}
void PointCloudPanel::remember_generated(const std::shared_ptr<const minirender::MeshData>& mesh,
                                         const std::string& object_id) {
    std::erase_if(generated_, [](const auto& value) { return value.expired(); });
    generated_.push_back(mesh);
    if (source_.object_id.empty()) {
        output_object_id_ = object_id;
    }
}
bool PointCloudPanel::allows_mesh(const std::shared_ptr<const minirender::MeshData>& mesh) const {
    return mesh && (mesh == source_.sampled_mesh ||
                    std::any_of(generated_.begin(), generated_.end(),
                                [&](const auto& value) { return value.lock() == mesh; }));
}
std::shared_ptr<const minirender::PointCloud> PointCloudPanel::display_cloud() const {
    return normals_ ? normals_->value.cloud : source_.cloud;
}

minirender::PointNormalSettings PointCloudPanel::normal_parameters() const {
    minirender::PointNormalSettings settings;
    settings.neighbors = static_cast<std::size_t>(neighbors_->value());
    settings.maximum_distance =
        distance_->value() == 0 ? std::numeric_limits<double>::infinity() : distance_->value();
    settings.estimate = estimate_->isChecked();
    settings.orient = orient_->isChecked();
    settings.flip = flip_->isChecked();
    settings.validate();
    return settings;
}
PointReconstructionInput PointCloudPanel::reconstruction_parameters() const {
    PointReconstructionInput input;
    input.cloud = source_.cloud;
    input.normals = normal_parameters();
    input.cached_normals = normals_;
    input.automatic_radii = automatic_->isChecked();
    input.balls.maximum_triangles = static_cast<std::size_t>(faces_->value());
    input.balls.maximum_candidate_tests = static_cast<std::size_t>(work_->value());
    if (!input.automatic_radii) {
        input.balls.radii.clear();
        for (const auto& text :
             radii_->text().split(QRegularExpression("[,，;\\s]+"), Qt::SkipEmptyParts)) {
            bool valid = false;
            const double value = QLocale::c().toDouble(text, &valid);
            if (!valid) {
                throw std::invalid_argument("半径须使用点号小数，用逗号或空格分隔。");
            }
            input.balls.radii.push_back(value);
        }
    }
    input.balls.validate();
    return input;
}
minirender::PointPreviewSettings PointCloudPanel::display_parameters() const {
    return {static_cast<float>(point_size_->value() * .5),
            static_cast<float>(normal_length_->value() * .02), normal_colors_->isChecked(),
            show_normals_->isChecked(), show_boundary_->isChecked()};
}
void PointCloudPanel::update_suggested_radii() {
    if (automatic_->isChecked() && normals_) {
        const double spacing = normals_->value.spacing_p90;
        radii_->setText(QString("%1, %2, %3")
                            .arg(2 * spacing, 0, 'g', 8)
                            .arg(3 * spacing, 0, 'g', 8)
                            .arg(4 * spacing, 0, 'g', 8));
    }
}
void PointCloudPanel::update_metrics() {
    QString text;
    if (normals_) {
        const auto& n = normals_->value;
        text += QString("法线结果：%1 个唯一点，合并 %2 个重复点\n本次邻域 %3 点，%4，翻转 %5 点\n"
                        "间距 P10 / 中位数 / P90：%6 / %7 / %8 m\n")
                    .arg(n.cloud->positions.size())
                    .arg(n.duplicate_points)
                    .arg(normals_->settings.neighbors)
                    .arg(normals_->settings.orient
                             ? QString("定向分量 %1").arg(n.orientation_components)
                             : QString("保留独立方向"))
                    .arg(n.reversed_normals)
                    .arg(n.spacing_p10, 0, 'g', 6)
                    .arg(n.spacing_median, 0, 'g', 6)
                    .arg(n.spacing_p90, 0, 'g', 6);
    }
    if (result_) {
        const auto& r = *result_;
        text += QString("重建结果：%1 面，%2 个输出顶点\n使用点 %3，未使用点 %4，额外面扇顶点 %5\n"
                        "面分量 %6，边界环 %7，边界边 %8，Euler %9\n候选检查 %10\n")
                    .arg(r.topology.faces)
                    .arg(r.mesh->positions.size())
                    .arg(r.used_points)
                    .arg(r.unused_points)
                    .arg(r.split_vertices)
                    .arg(r.topology.face_components)
                    .arg(r.topology.boundary_loops)
                    .arg(r.topology.boundary_edges)
                    .arg(r.topology.euler_characteristic)
                    .arg(r.candidate_tests);
        for (const auto& pass : r.passes) {
            text += QString("r=%1 m：新增 %2 面，边界边 %3，激活 %4\n")
                        .arg(pass.radius, 0, 'g', 8)
                        .arg(pass.new_triangles)
                        .arg(pass.boundary_edges)
                        .arg(pass.restarted_edges);
        }
    }
    if (preview_) {
        text += QString("预览：%1 个点，%2 条抽样法线，%3 条边界边")
                    .arg(preview_->points.size())
                    .arg(preview_->normal_samples.size())
                    .arg(preview_->boundary_edges);
    }
    metrics_->setText(text);
}
void PointCloudPanel::update_state() {
    const auto status = controller_.status();
    const bool running = controller_.busy();
    const bool blocked = running || external_busy_;
    const bool source = source_.cloud != nullptr;
    QString warning;
    bool valid = source;
    try {
        const auto parameters = reconstruction_parameters();
        if (source && !parameters.normals.estimate && source_.cloud->normals.empty()) {
            valid = false;
            warning = "原始点云没有法线，请启用估计。";
        }
    } catch (const std::exception& error) {
        valid = false;
        warning = QString::fromUtf8(error.what());
    }
    if (source && !target_valid_) {
        valid = false;
        warning = "来源物体或网格已改变，请重新采样。";
    }
    source_label_->setText(
        source ? QString("当前点云：%1\n原始点数 %2；%3")
                     .arg(QString::fromStdString(source_.name))
                     .arg(source_.cloud->positions.size())
                     .arg(will_create_ ? "重建将新建场景物体" : "重建将更新目标物体的网格")
               : "导入 PLY，或从场景中选取网格进行表面采样。");
    warning_->setText(warning);
    warning_->setVisible(!warning.isEmpty());
    import_->setEnabled(!blocked);
    sample_->setEnabled(!blocked && selected_mesh_);
    prepare_->setEnabled(!blocked && source &&
                         (estimate_->isChecked() || !source_.cloud->normals.empty()));
    reconstruct_->setEnabled(!blocked && valid);
    export_->setEnabled(!blocked && source);
    cancel_->setEnabled(running && status.state != GeometryTaskState::cancelling);
    visible_->setEnabled(preview_ != nullptr);
    locate_->setEnabled(preview_ != nullptr);
    point_size_->setEnabled(preview_ != nullptr);
    normal_colors_->setEnabled(preview_ && preview_->has_normals);
    show_normals_->setEnabled(preview_ && !preview_->normal_samples.empty());
    show_boundary_->setEnabled(preview_ && preview_->boundary_edges > 0);
    radii_->setEnabled(!automatic_->isChecked());
    fit_->setEnabled(will_create_);
    if (running) {
        stage_->setText(QString::fromUtf8(status.progress.stage.c_str()));
        if (status.progress.total > 1) {
            progress_->setRange(0, 1000);
            progress_->setValue(
                static_cast<int>(1000 * status.progress.completed / status.progress.total));
            progress_->setFormat("阶段 %p%");
            counts_->setText(
                QString("%1 / %2").arg(status.progress.completed).arg(status.progress.total));
        } else {
            progress_->setRange(0, 0);
            counts_->clear();
        }
    } else {
        progress_->setRange(0, 1);
        progress_->setValue(status.state == GeometryTaskState::ready ? 1 : 0);
        progress_->setFormat(status.state == GeometryTaskState::ready ? "完成" : "");
        stage_->setText(QString::fromUtf8(status.message.c_str()));
        counts_->setText(status.jobs_started
                             ? QString("任务耗时 %1 s")
                                   .arg(static_cast<double>(status.elapsed_ms) / 1000.0, 0, 'f', 2)
                             : "");
    }
}
