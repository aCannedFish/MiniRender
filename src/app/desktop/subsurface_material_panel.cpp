#include "subsurface_material_panel.hpp"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>

namespace {
// 原生输入和步进保留，短数字显示避免三个 RGB 字段被补零撑宽。
class Scalar final : public QDoubleSpinBox {
  public:
    using QDoubleSpinBox::QDoubleSpinBox;

  protected:
    QString textFromValue(double value) const override {
        return locale().toString(value, 'g', QLocale::FloatingPointShortest);
    }
};
QDoubleSpinBox* scalar(QWidget* parent, const QString& name, double low, double high, double step) {
    auto* field = new Scalar(parent);
    field->setObjectName(name);
    field->setLocale(QLocale::c());
    field->setDecimals(323);
    field->setRange(low, high);
    field->setSingleStep(step);
    field->setKeyboardTracking(false);
    field->setMinimumWidth(72);
    field->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    return field;
}
void display(QDoubleSpinBox* field, double value) {
    const QSignalBlocker blocker(field);
    field->setValue(value);
    field->setToolTip("完整参数：" +
                      QLocale::c().toString(value, 'g', QLocale::FloatingPointShortest));
}
} // namespace

SubsurfaceMaterialPanel::SubsurfaceMaterialPanel(QWidget* parent) : QWidget(parent) {
    setObjectName("subsurface_material_panel");
    auto* form = new QFormLayout(this);
    form->setContentsMargins(0, 0, 0, 0);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto* hint = new QLabel("SSS 使用闭合、连通且朝外绕序的网格。实时视口使用表面 PBR 近似，"
                            "跨位置扩散通过离线渲染显示。密度改变扩散距离，实时近似底色保持不变。",
                            this);
    hint->setObjectName("subsurface_hint");
    hint->setWordWrap(true);
    form->addRow(hint);
    preset_ = new QComboBox(this);
    preset_->setObjectName("subsurface_preset");
    preset_->addItem("大理石（测量）", "marble");
    preset_->addItem("蜡（示例）", "wax");
    auto* preset_button = new QPushButton("使用预置", this);
    preset_button->setObjectName("subsurface_apply_preset");
    auto* preset_row = new QWidget(this);
    auto* preset_layout = new QHBoxLayout(preset_row);
    preset_layout->setContentsMargins(0, 0, 0, 0);
    preset_layout->addWidget(preset_, 1);
    preset_layout->addWidget(preset_button);
    form->addRow("光学预置", preset_row);
    form->addRow("吸收 / m⁻¹", vector_field("subsurface_absorption", absorption_, true));
    form->addRow("约化散射 / m⁻¹", vector_field("subsurface_scattering", scattering_, false));
    density_ = scalar(this, "subsurface_density", 1e-6, 1e6, .1);
    form->addRow("密度倍率", density_);
    connect(density_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        if (draft_) {
            draft_->density = value;
            error_.clear();
            update_buttons();
        }
    });
    apply_ = new QPushButton("应用光学参数", this);
    apply_->setObjectName("subsurface_apply");
    restore_ = new QPushButton("恢复草稿", this);
    restore_->setObjectName("subsurface_restore");
    auto* buttons = new QWidget(this);
    auto* button_layout = new QHBoxLayout(buttons);
    button_layout->setContentsMargins(0, 0, 0, 0);
    button_layout->addWidget(apply_);
    button_layout->addWidget(restore_);
    form->addRow(buttons);
    render_ = new QPushButton("用 PT 开始离线渲染", this);
    render_->setObjectName("subsurface_render");
    form->addRow(render_);
    notice_ = new QLabel(this);
    notice_->setObjectName("subsurface_notice");
    notice_->setWordWrap(true);
    form->addRow(notice_);
    connect(apply_, &QPushButton::clicked, this, [this] { apply(); });
    connect(restore_, &QPushButton::clicked, this, [this] {
        draft_ = source_;
        error_.clear();
        refresh_fields();
    });
    connect(render_, &QPushButton::clicked, this, [this] {
        if (on_render) {
            on_render(!viewing_task_);
        }
        update_buttons();
    });
    connect(preset_button, &QPushButton::clicked, this, [this] {
        const auto preset =
            minirender::make_subsurface_preset(preset_->currentData().toString().toStdString());
        draft_ = *preset.parameters;
        error_.clear();
        if (on_changed) {
            on_changed(preset.parameters, preset.ior);
        }
        refresh_fields();
    });
    auto* timer = new QTimer(this);
    timer->setInterval(200);
    connect(timer, &QTimer::timeout, this, [this] { update_buttons(); });
    timer->start();
    set_material(nullptr, 0);
}
QWidget* SubsurfaceMaterialPanel::vector_field(const char* name,
                                               std::array<QDoubleSpinBox*, 3>& fields,
                                               bool absorption) {
    auto* row = new QWidget(this);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    for (int c = 0; c < 3; ++c) {
        layout->addWidget(new QLabel(QString("RGB").mid(c, 1), row));
        auto* field = scalar(row, QString("%1.%2").arg(name).arg(c), 0, 1e6, 1);
        fields[std::size_t(c)] = field;
        layout->addWidget(field, 1);
        connect(field, &QDoubleSpinBox::valueChanged, this, [this, c, absorption](double value) {
            if (draft_) {
                (absorption ? draft_->absorption : draft_->reduced_scattering)[c] = value;
                error_.clear();
                update_buttons();
            }
        });
    }
    return row;
}
void SubsurfaceMaterialPanel::set_material(const minirender::Material* material,
                                           std::uint64_t generation) {
    const bool valid = material && material->kind == minirender::MaterialKind::subsurface;
    setVisible(valid);
    if (!valid) {
        source_.reset();
        draft_.reset();
        material_id_.clear();
        return;
    }
    const auto& source = *material->subsurface;
    if (!source_ || material_id_ != material->id || generation_ != generation ||
        *source_ != source) {
        material_id_ = material->id;
        generation_ = generation;
        source_ = draft_ = source;
        error_.clear();
        refresh_fields();
    }
}
bool SubsurfaceMaterialPanel::dirty() const {
    return source_ && draft_ && *source_ != *draft_;
}
void SubsurfaceMaterialPanel::refresh_fields() {
    if (!draft_) {
        return;
    }
    for (std::size_t c = 0; c < 3; ++c) {
        display(absorption_[c], draft_->absorption[int(c)]);
        display(scattering_[c], draft_->reduced_scattering[int(c)]);
    }
    display(density_, draft_->density);
    update_buttons();
}
void SubsurfaceMaterialPanel::update_buttons() {
    const bool active = render_active && render_active();
    viewing_task_ = active;
    apply_->setEnabled(dirty());
    restore_->setEnabled(dirty());
    render_->setEnabled(draft_.has_value() && (active || !dirty()));
    render_->setText(active ? "查看离线任务" : "用 PT 开始离线渲染");
    notice_->setText(!error_.isEmpty() ? error_
                     : dirty()         ? "草稿尚未应用，离线渲染使用已提交参数。"
                                       : "");
}
void SubsurfaceMaterialPanel::apply() {
    try {
        draft_->validate();
        if (on_changed) {
            on_changed(std::make_shared<const minirender::SubsurfaceParameters>(*draft_), {});
        }
        error_.clear();
    } catch (const std::exception& error) {
        error_ = QString::fromUtf8(error.what());
    }
    update_buttons();
}
