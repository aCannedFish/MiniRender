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
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFormAlignment(Qt::AlignTop | Qt::AlignLeft);
    form->setContentsMargins(10, 12, 10, 12);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(8);
    target_ = text_label(this, "geometry_target");
    form->addRow(target_);
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
    if (mesh_) {
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
