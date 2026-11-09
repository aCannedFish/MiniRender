#include "geometry_panel.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>

#include <algorithm>

namespace {

QLabel* text_label(QWidget* parent, const QString& name) {
    auto* label = new QLabel(parent);
    label->setObjectName(name);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    label->setMinimumWidth(0);
    return label;
}

QDoubleSpinBox* scalar(QWidget* parent, const QString& name, double minimum, double maximum,
                       double value, int decimals, double step) {
    auto* field = new QDoubleSpinBox(parent);
    field->setObjectName(name);
    field->setRange(minimum, maximum);
    field->setDecimals(decimals);
    field->setSingleStep(step);
    field->setValue(value);
    field->setKeyboardTracking(false);
    field->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return field;
}

} // namespace

GeometryPanel::GeometryPanel(GeometryController& controller, QWidget* parent)
    : QWidget(parent), controller_(controller) {
    setObjectName("geometry_panel");
    auto* form = new QFormLayout(this);
    form_ = form;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFormAlignment(Qt::AlignTop | Qt::AlignLeft);
    form->setContentsMargins(10, 12, 10, 12);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(8);
    target_ = text_label(this, "geometry_target");
    form->addRow(target_);
    operation_ = new QComboBox(this);
    operation_->setObjectName("geometry_operation");
    operation_->addItems({"Loop 与高度位移", "QEM 网格简化", "各向同性重网格"});
    form->addRow("操作", operation_);
    remeshing_ = new QWidget(this);
    auto* remesh_form = new QFormLayout(remeshing_);
    remesh_form->setContentsMargins(0, 0, 0, 0);
    remesh_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    remesh_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    remesh_length_ = scalar(remeshing_, "geometry_remesh_length", 0.000001, 10000, 0.1, 6, 0.01);
    remesh_length_->setSuffix(" m");
    remesh_step_ = scalar(remeshing_, "geometry_remesh_smoothing", 0, 1, 0.5, 2, 0.1);
    remesh_iterations_ = new QSpinBox(remeshing_);
    remesh_iterations_->setObjectName("geometry_remesh_iterations");
    remesh_iterations_->setRange(0, 20);
    remesh_iterations_->setValue(5);
    remesh_iterations_->setKeyboardTracking(false);
    remesh_budget_ = new QSpinBox(remeshing_);
    remesh_budget_->setObjectName("geometry_remesh_budget");
    remesh_budget_->setRange(1, 1000000);
    remesh_budget_->setValue(500000);
    remesh_budget_->setKeyboardTracking(false);
    remesh_projection_ = new QCheckBox("投影到原表面并传递 UV", remeshing_);
    remesh_projection_->setObjectName("geometry_remesh_projection");
    remesh_projection_->setChecked(true);
    remesh_form->addRow("目标边长", remesh_length_);
    remesh_form->addRow("迭代次数", remesh_iterations_);
    remesh_form->addRow("面数上限", remesh_budget_);
    remesh_form->addRow("平滑步长", remesh_step_);
    remesh_form->addRow(remesh_projection_);
    form->addRow(remeshing_);
    constraints_ = new QWidget(this);
    auto* simplify_form = new QFormLayout(constraints_);
    tool_form_ = simplify_form;
    simplify_form->setContentsMargins(0, 0, 0, 0);
    simplify_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    simplify_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    target_faces_ = new QSpinBox(constraints_);
    target_faces_->setObjectName("geometry_target_faces");
    target_faces_->setRange(1, 2000000);
    target_faces_->setValue(1000);
    target_faces_->setKeyboardTracking(false);
    simplify_form->addRow("目标面数", target_faces_);
    const auto toggle = [&](const char* name, const char* label) {
        auto* field = new QCheckBox(QString::fromUtf8(label), constraints_);
        field->setObjectName(name);
        field->setChecked(true);
        simplify_form->addRow(field);
        connect(field, &QCheckBox::toggled, this, [this] { update(); });
        return field;
    };
    boundary_ = toggle("geometry_preserve_boundary", "固定边界位置");
    uv_seams_ = toggle("geometry_preserve_uv", "保护 UV 接缝");
    hard_normals_ = toggle("geometry_preserve_normals", "保护硬法线");
    creases_ = toggle("geometry_preserve_creases", "固定几何折痕");
    crease_angle_ = scalar(constraints_, "geometry_crease_angle", 1, 180, 60, 1, 5);
    crease_angle_->setSuffix(" °");
    simplify_form->addRow("折痕夹角", crease_angle_);
    error_limit_ = toggle("geometry_limit_error", "限制 QEM 代价");
    quadric_error_ = scalar(constraints_, "geometry_quadric_error", 0, 1000000, 0.01, 9, 0.001);
    quadric_error_->setSuffix(" m²");
    simplify_form->addRow("代价上限", quadric_error_);
    // 控件建立完毕后再启用 update；构造中不触发未初始化成员的读取。
    error_limit_->blockSignals(true);
    error_limit_->setChecked(false);
    error_limit_->blockSignals(false);
    form->addRow(constraints_);
    levels_ = new QSpinBox(this);
    levels_->setObjectName("geometry_levels");
    levels_->setRange(0, 8);
    levels_->setValue(2);
    levels_->setKeyboardTracking(false);
    form->addRow("Loop 层数", levels_);
    budget_ = new QSpinBox(this);
    budget_->setObjectName("geometry_budget");
    budget_->setRange(1, 2000000);
    budget_->setValue(2000000);
    budget_->setSingleStep(10000);
    budget_->setKeyboardTracking(false);
    form->addRow("面数上限", budget_);
    estimate_ = text_label(this, "geometry_estimate");
    form->addRow(estimate_);
    displace_ = new QCheckBox("启用高度位移", this);
    displace_->setObjectName("geometry_displacement");
    form->addRow(displace_);
    auto* height_row = new QWidget(this);
    height_row_ = height_row;
    auto* height_layout = new QHBoxLayout(height_row);
    height_layout->setContentsMargins(0, 0, 0, 0);
    height_layout->setSpacing(6);
    height_ = new QComboBox(height_row);
    height_->setObjectName("geometry_height");
    height_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    height_->setMinimumContentsLength(6);
    height_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    height_->addItem("无", QString());
    import_ = new QPushButton("导入高度图", height_row);
    import_->setObjectName("geometry_import_height");
    height_layout->addWidget(height_, 1);
    height_layout->addWidget(import_);
    form->addRow("线性高度图", height_row);
    scale_ = scalar(this, "geometry_scale", -100, 100, 0.1, 4, 0.01);
    scale_->setSuffix(" m");
    midlevel_ = scalar(this, "geometry_midlevel", -1000000, 1000000, 0.5, 4, 0.1);
    tolerance_ = scalar(this, "geometry_tolerance", 0, 1, 1e-6, 8, 1e-6);
    tolerance_->setSuffix(" m");
    form->addRow("位移幅度", scale_);
    form->addRow("高度中值", midlevel_);
    form->addRow("接缝容差", tolerance_);
    auto* note = text_label(this, "geometry_note");
    note_ = note;
    note->setText("高度取线性 R 通道，距离为物体局部米。生成基于当前网格；再次生成会继续细分，"
                  "可通过撤销恢复。原始共享资产保留，仅更新启动任务时选中的物体。");
    form->addRow(note);
    auto* buttons = new QWidget(this);
    auto* row = new QHBoxLayout(buttons);
    row->setContentsMargins(0, 0, 0, 0);
    apply_ = new QPushButton("生成网格", buttons);
    apply_->setObjectName("geometry_apply");
    export_ = new QPushButton("导出 OBJ", buttons);
    export_->setObjectName("geometry_export");
    cancel_ = new QPushButton("取消", buttons);
    cancel_->setObjectName("geometry_cancel");
    row->addWidget(apply_);
    row->addWidget(export_);
    row->addWidget(cancel_);
    form->addRow(buttons);
    stage_ = text_label(this, "geometry_stage");
    counts_ = text_label(this, "geometry_counts");
    progress_ = new QProgressBar(this);
    progress_->setObjectName("geometry_progress");
    progress_->setRange(0, 1000);
    progress_->setValue(0);
    result_ = text_label(this, "geometry_result");
    metrics_ = text_label(this, "geometry_metrics");
    form->addRow(stage_);
    form->addRow(progress_);
    form->addRow(counts_);
    form->addRow(result_);
    form->addRow(metrics_);
    for (auto* field : {scale_, midlevel_, tolerance_}) {
        connect(field, &QDoubleSpinBox::valueChanged, this, [this] { update(); });
    }
    connect(levels_, &QSpinBox::valueChanged, this, [this] { update(); });
    connect(budget_, &QSpinBox::valueChanged, this, [this] { update(); });
    connect(displace_, &QCheckBox::toggled, this, [this] { update(); });
    connect(height_, &QComboBox::currentIndexChanged, this, [this] { update(); });
    connect(operation_, &QComboBox::currentIndexChanged, this, [this] { update(); });
    connect(target_faces_, &QSpinBox::valueChanged, this, [this] { update(); });
    connect(quadric_error_, &QDoubleSpinBox::valueChanged, this, [this] { update(); });
    connect(crease_angle_, &QDoubleSpinBox::valueChanged, this, [this] { update(); });
    connect(remesh_length_, &QDoubleSpinBox::valueChanged, this, [this] { update(); });
    connect(remesh_step_, &QDoubleSpinBox::valueChanged, this, [this] { update(); });
    connect(remesh_iterations_, &QSpinBox::valueChanged, this, [this] { update(); });
    connect(remesh_budget_, &QSpinBox::valueChanged, this, [this] { update(); });
    connect(remesh_projection_, &QCheckBox::toggled, this, [this] { update(); });
    connect(apply_, &QPushButton::clicked, this, [this] {
        if (on_apply) {
            on_apply();
        }
    });
    connect(import_, &QPushButton::clicked, this, [this] {
        if (on_import) {
            on_import();
        }
    });
    connect(export_, &QPushButton::clicked, this, [this] {
        if (on_export) {
            on_export();
        }
    });
    connect(cancel_, &QPushButton::clicked, this, [this] { controller_.cancel(); });
    auto* timer = new QTimer(this);
    timer->setInterval(50);
    connect(timer, &QTimer::timeout, this, [this] { update(); });
    timer->start();
    update();
}

GeometryParameters GeometryPanel::parameters() const {
    GeometryParameters result;
    if (operation_->currentIndex() == 2) {
        minirender::RemeshSettings settings;
        settings.target_edge_length = remesh_length_->value();
        settings.iterations = static_cast<std::uint32_t>(remesh_iterations_->value());
        settings.maximum_triangles = static_cast<std::size_t>(remesh_budget_->value());
        settings.smoothing_step = remesh_step_->value();
        settings.project_to_surface = remesh_projection_->isChecked();
        settings.preserve_boundary = boundary_->isChecked();
        settings.preserve_uv_seams = uv_seams_->isChecked();
        settings.preserve_hard_normals = hard_normals_->isChecked();
        settings.preserve_creases = creases_->isChecked();
        settings.crease_angle_degrees = crease_angle_->value();
        result.remesh = settings;
        return result;
    }
    if (operation_->currentIndex() == 1) {
        minirender::SimplifySettings settings;
        settings.target_triangles = static_cast<std::size_t>(target_faces_->value());
        settings.preserve_boundary = boundary_->isChecked();
        settings.preserve_uv_seams = uv_seams_->isChecked();
        settings.preserve_hard_normals = hard_normals_->isChecked();
        settings.preserve_creases = creases_->isChecked();
        settings.crease_angle_degrees = crease_angle_->value();
        if (error_limit_->isChecked()) {
            settings.maximum_quadric_error = quadric_error_->value();
        }
        result.simplify = settings;
        return result;
    }
    result.loop.levels = static_cast<std::uint32_t>(levels_->value());
    result.loop.maximum_triangles = static_cast<std::size_t>(budget_->value());
    result.height_id = height_->currentData().toString().toStdString();
    if (displace_->isChecked()) {
        result.displacement = minirender::DisplacementSettings{};
        result.displacement->scale = scale_->value();
        result.displacement->midlevel = midlevel_->value();
        result.displacement->seam_tolerance = tolerance_->value();
    }
    return result;
}

void GeometryPanel::set_context(const minirender::SceneDocument& document,
                                const std::string& selected_id) {
    const auto object = std::find_if(document.objects.begin(), document.objects.end(),
                                     [&](const auto& item) { return item.id == selected_id; });
    const auto old = mesh_;
    mesh_.reset();
    if (object != document.objects.end()) {
        const auto mesh =
            std::find_if(document.meshes.begin(), document.meshes.end(),
                         [&](const auto& item) { return item.id == object->mesh_id; });
        if (mesh != document.meshes.end()) {
            mesh_ = mesh->data;
            target_->setText(QString("目标：%1\n当前网格：%2 个位置，%3 个三角形")
                                 .arg(QString::fromStdString(object->name))
                                 .arg(mesh_->positions.size())
                                 .arg(mesh_->triangles.size()));
        }
    }
    if (!mesh_) {
        target_->setText("请先选择一个物体");
    }
    if (old != mesh_) {
        metrics_->clear();
    }
    const auto chosen = height_->currentData();
    const QSignalBlocker blocker(height_);
    height_->clear();
    height_->addItem("无", QString());
    for (const auto& texture : document.textures) {
        if (texture.encoding == minirender::ColorEncoding::linear) {
            height_->addItem(QString::fromStdString(texture.id),
                             QString::fromStdString(texture.id));
        }
    }
    height_->setCurrentIndex(std::max(0, height_->findData(chosen)));
    update();
}

void GeometryPanel::set_asset_task_busy(bool busy) {
    external_busy_ = busy;
    update();
}
void GeometryPanel::select_height(const std::string& id) {
    height_->setCurrentIndex(std::max(0, height_->findData(QString::fromStdString(id))));
    displace_->setChecked(true);
    update();
}
void GeometryPanel::show_result(const GeometryTaskResult& result) {
    if (result.remeshing) {
        const auto& r = *result.remeshing;
        const double fraction = r.after.edges
                                    ? 100.0 * static_cast<double>(r.after.in_target_range) /
                                          static_cast<double>(r.after.edges)
                                    : 0;
        metrics_->setText(
            QString(
                "本次边长 %23 m，完成 %1 轮：%2 → %3 面\n分裂 %4，折叠 %5，翻边 %6，顶点更新 %7\n"
                "目标区间内：%8 / %9 条边（%10%）\n"
                "边长 P10-P90：%11-%12 → %13-%14 m\n"
                "最小角：%15° → %16°\n平均角度偏差：%21° → %22°\n双向 RMS：%17 "
                "m，采样最大距离：%18 m\n保护 "
                "%19 个位置，拒绝平滑更新 %20 次")
                .arg(r.iterations_completed)
                .arg(r.input_topology.faces)
                .arg(r.topology.faces)
                .arg(r.splits)
                .arg(r.collapses)
                .arg(r.flips)
                .arg(r.vertex_updates)
                .arg(r.after.in_target_range)
                .arg(r.after.edges)
                .arg(fraction, 0, 'f', 1)
                .arg(r.before.edge_p10, 0, 'g', 5)
                .arg(r.before.edge_p90, 0, 'g', 5)
                .arg(r.after.edge_p10, 0, 'g', 5)
                .arg(r.after.edge_p90, 0, 'g', 5)
                .arg(r.before.minimum_angle, 0, 'f', 2)
                .arg(r.after.minimum_angle, 0, 'f', 2)
                .arg(r.error.symmetric_rms, 0, 'g', 5)
                .arg(r.error.sampled_maximum, 0, 'g', 5)
                .arg(r.protected_vertices)
                .arg(r.rejected_moves)
                .arg(r.before.mean_angle_deviation, 0, 'f', 2)
                .arg(r.after.mean_angle_deviation, 0, 'f', 2)
                .arg(r.target_edge_length, 0, 'g', 6));
        return;
    }
    if (result.simplification) {
        const auto& s = *result.simplification;
        const QString reason = s.stop == minirender::SimplifyStop::target_reached ? "已达到目标"
                               : s.stop == minirender::SimplifyStop::error_limit
                                   ? "已达 QEM 代价上限"
                                   : "剩余边受拓扑或属性约束";
        metrics_->setText(QString("目标 %1 面，实际 %2 → %3 面\n%4，折叠 %5 条边，保护 %6 个顶点\n"
                                  "最大折叠代价 %7 m²\n双向 RMS %8 m，采样最大距离 %9 m\n"
                                  "原表面 %10 个样本，结果表面 %11 个样本")
                              .arg(s.requested_triangles)
                              .arg(s.input_topology.faces)
                              .arg(s.topology.faces)
                              .arg(reason)
                              .arg(s.collapses)
                              .arg(s.protected_vertices)
                              .arg(s.maximum_quadric_error, 0, 'g', 6)
                              .arg(s.error.symmetric_rms, 0, 'g', 6)
                              .arg(s.error.sampled_maximum, 0, 'g', 6)
                              .arg(s.error.original_to_result.samples)
                              .arg(s.error.result_to_original.samples));
        return;
    }
    metrics_->setText(
        QString("结果：%1 个位置，%2 个三角形，%3 条边界边\n最大实际位移：%4 m")
            .arg(result.topology.referenced_vertices + result.topology.isolated_vertices)
            .arg(result.topology.faces)
            .arg(result.topology.boundary_edges)
            .arg(result.maximum_displacement, 0, 'g', 6));
}

void GeometryPanel::update() {
    const auto status = controller_.status();
    const bool running = controller_.busy();
    const bool blocked = running || external_busy_;
    bool permitted = mesh_ != nullptr;
    const auto params = parameters();
    const bool simplifying = params.simplify.has_value();
    const bool remeshing = params.remesh.has_value();
    for (QWidget* field : std::array<QWidget*, 7>{levels_, budget_, displace_, height_row_, scale_,
                                                  midlevel_, tolerance_}) {
        form_->setRowVisible(field, !simplifying && !remeshing);
    }
    form_->setRowVisible(constraints_, simplifying || remeshing);
    form_->setRowVisible(remeshing_, remeshing);
    tool_form_->setRowVisible(target_faces_, simplifying);
    tool_form_->setRowVisible(error_limit_, simplifying);
    tool_form_->setRowVisible(quadric_error_, simplifying);
    crease_angle_->setEnabled(creases_->isChecked());
    quadric_error_->setEnabled(error_limit_->isChecked());
    note_->setText(
        remeshing ? "按物体局部边长调整三角形分布，依次分裂、折叠、翻边、平滑和投影。"
                    "保护边可细分，原位置保留；结果报告实际边长分布与表面误差。"
        : simplifying
            ? "在当前网格上执行边折叠。接缝与硬法线顶点的相邻边保留，边界与折痕位置固定。"
              "结果报告实际面数和双向采样误差；保护条件可能使目标面数不可达。"
            : "高度取线性 R 通道，距离为物体局部米。生成基于当前网格；再次生成会继续细分，"
              "可通过撤销恢复。原始共享资产保留，仅更新启动任务时选中的物体。");
    if (mesh_ && !simplifying && !remeshing) {
        std::uint64_t faces = mesh_->triangles.size(), positions = mesh_->positions.size();
        bool over_budget = faces > params.loop.maximum_triangles;
        for (std::uint32_t level = 0; level < params.loop.levels && !over_budget; ++level) {
            over_budget = faces > params.loop.maximum_triangles / 4;
            if (!over_budget) {
                positions += 3 * faces;
                faces *= 4;
            }
        }
        // 包含新旧 CPU 数据、邻接/法线临时量与 GPU 缓冲的保守上界，不扫描网格。
        const auto estimated = faces * 768 + positions * 96 + mesh_->triangles.size() * 128;
        const bool over_memory = estimated > 2ULL * 1024 * 1024 * 1024;
        permitted = !over_budget && !over_memory;
        estimate_->setText(over_budget
                               ? "预计面数超过上限，请减少层数或调整面数上限。"
                               : QString("预计 %1 个三角形，工作内存约 %2 MiB（不含已有资产）。%3")
                                     .arg(faces)
                                     .arg(estimated / (1024 * 1024))
                                     .arg(over_memory ? "超过 2 GiB 预算，请减少层数。" : ""));
    } else {
        estimate_->clear();
    }
    if (mesh_ && simplifying) {
        const auto faces = mesh_->triangles.size();
        const auto estimated = faces * 1536 + mesh_->positions.size() * 256;
        permitted = faces <= 2000000 && estimated <= 2ULL * 1024 * 1024 * 1024;
        estimate_->setText(QString("输入 %1 面，目标 %2 面。工作内存估计 %3 MiB。%4")
                               .arg(faces)
                               .arg(params.simplify->target_triangles)
                               .arg(static_cast<double>(estimated) / (1024 * 1024), 0, 'f', 1)
                               .arg(permitted ? "" : "超过几何任务预算，请先缩小输入。"));
    }
    if (mesh_ && remeshing) {
        const auto faces = mesh_->triangles.size();
        const auto estimated = faces * 1792 + mesh_->positions.size() * 256;
        permitted =
            faces <= params.remesh->maximum_triangles && estimated <= 2ULL * 1024 * 1024 * 1024;
        estimate_->setText(QString("输入 %1 面，执行中最多 %2 面；输入工作集估计 %3 MiB。%4")
                               .arg(faces)
                               .arg(params.remesh->maximum_triangles)
                               .arg(static_cast<double>(estimated) / (1024 * 1024), 0, 'f', 1)
                               .arg(permitted ? "" : "输入超过面数或工作内存预算。"));
    }
    const bool height_needed = params.displacement && params.displacement->scale != 0;
    apply_->setEnabled(permitted && !blocked && (!height_needed || !params.height_id.empty()));
    export_->setEnabled(mesh_ && !blocked);
    import_->setEnabled(!blocked);
    cancel_->setEnabled(running && status.state != GeometryTaskState::cancelling);
    height_->setEnabled(displace_->isChecked() && !blocked);
    for (auto* field : {scale_, midlevel_, tolerance_}) {
        field->setEnabled(displace_->isChecked());
    }
    if (running) {
        stage_->setText(QString::fromUtf8(status.progress.stage.c_str()));
        const auto& p = status.progress;
        if (p.total > 1) {
            progress_->setRange(0, 1000);
            progress_->setValue(static_cast<int>(1000 * p.completed / p.total));
            progress_->setFormat("%p%");
            counts_->setText(QString("当前阶段：%1 / %2").arg(p.completed).arg(p.total));
        } else {
            progress_->setRange(0, 0);
            counts_->clear();
        }
        result_->setText(status.state == GeometryTaskState::cancelling ? "正在取消…" : "");
    } else {
        progress_->setRange(0, 1);
        const bool ready = status.state == GeometryTaskState::ready;
        progress_->setValue(ready ? 1 : 0);
        progress_->setFormat(ready                                       ? "完成"
                             : status.state == GeometryTaskState::failed ? "失败"
                                                                         : "");
        stage_->setText(external_busy_ ? "正在处理其他资产任务…"
                        : status.jobs_started
                            ? QString("任务耗时：%1 s")
                                  .arg(static_cast<double>(status.elapsed_ms) / 1000.0, 0, 'f', 2)
                            : "点击生成网格开始处理");
        counts_->clear();
        result_->setText(QString::fromUtf8(status.message.c_str()));
    }
}
