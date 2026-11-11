#include "cloth_material_panel.hpp"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>

#include <stdexcept>

namespace {

QDoubleSpinBox* scalar(QWidget* parent, const QString& name, double low, double high,
                       double step = .1, int decimals = 5) {
    auto* spin = new QDoubleSpinBox(parent);
    spin->setObjectName(name);
    spin->setRange(low, high);
    spin->setDecimals(decimals);
    spin->setSingleStep(step);
    spin->setKeyboardTracking(false);
    spin->setLocale(QLocale::c());
    spin->setMinimumWidth(82);
    spin->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    return spin;
}
QFormLayout* form(QWidget* widget) {
    auto* layout = new QFormLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setVerticalSpacing(12);
    layout->setRowWrapPolicy(QFormLayout::WrapLongRows);
    layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    return layout;
}
QString curve_text(const std::vector<double>& values) {
    QStringList text;
    for (const auto value : values) {
        text.push_back(QLocale::c().toString(value, 'g', QLocale::FloatingPointShortest));
    }
    return text.join(", ");
}
std::vector<double> curve_values(const QString& text) {
    const auto tokens = text.split(QRegularExpression("[,\\s]+"), Qt::SkipEmptyParts);
    if (tokens.empty() || tokens.size() > 16) {
        throw std::invalid_argument("曲线需要 1–16 个逗号或空格分隔的数值。");
    }
    std::vector<double> result;
    for (const auto& token : tokens) {
        bool ok = false;
        const double value = QLocale::c().toDouble(token, &ok);
        if (!ok) {
            throw std::invalid_argument("曲线包含无效数值。");
        }
        result.push_back(value);
    }
    return result;
}

} // namespace

ClothMaterialPanel::ClothMaterialPanel(QWidget* parent) : QWidget(parent) {
    setObjectName("cloth_material_panel");
    auto* layout = form(this);
    auto* hint = new QLabel(
        "底色和纹理调节内部散射，表面高光保持白色。实时环境光采用平均纱线颜色近似。", this);
    hint->setTextFormat(Qt::PlainText);
    hint->setWordWrap(true);
    layout->addRow(hint);
    preset_ = new QComboBox(this);
    preset_->setObjectName("cloth_preset");
    for (const auto* name : {"linen", "silk", "satin", "shot"}) {
        preset_->addItem(name, name);
    }
    auto* apply = new QPushButton("使用预置", this);
    apply->setObjectName("cloth_apply_preset");
    auto* row = new QWidget(this);
    auto* buttons = new QHBoxLayout(row);
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->addWidget(preset_, 1);
    buttons->addWidget(apply);
    layout->addRow("布料预置", row);
    apply->setToolTip("更新纱线参数、折射率和底色倍率。");
    connect(apply, &QPushButton::clicked, this, [this] {
        const auto preset =
            minirender::make_cloth_preset(preset_->currentData().toString().toStdString());
        for (auto& fields : threads_) {
            fields.angles->setModified(false);
            fields.lengths->setModified(false);
        }
        if (on_changed) {
            on_changed(preset.appearance, preset.ior);
        }
    });
    rotation_ = scalar(this, "cloth_rotation", -3600, 3600, 5);
    coverage_ = scalar(this, "cloth_coverage", .01, 1, .01);
    ratio_ = scalar(this, "cloth_warp_ratio", 0, 1, .01);
    correlation_ = scalar(this, "cloth_correlation", 1, 90, 1);
    samples_ = new QSpinBox(this);
    samples_->setObjectName("cloth_tangent_samples");
    samples_->setRange(1, 16);
    samples_->setKeyboardTracking(false);
    layout->addRow("方向旋转 °", rotation_);
    layout->addRow("总覆盖率", coverage_);
    layout->addRow("经向占比", ratio_);
    layout->addRow("相关宽度 °", correlation_);
    layout->addRow("切向采样", samples_);
    connect(rotation_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        edit([&](auto& settings) { settings.rotation_degrees = value; });
    });
    connect(correlation_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        edit([&](auto& settings) { settings.correlation_width_degrees = value; });
    });
    connect(samples_, &QSpinBox::valueChanged, this, [this](int value) {
        edit([&](auto& settings) {
            settings.samples_per_thread = static_cast<std::uint32_t>(value);
        });
    });
    connect(coverage_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        edit([&](auto& settings) {
            const double ratio =
                settings.coverage[0] / (settings.coverage[0] + settings.coverage[1]);
            settings.coverage[0] = value * ratio;
            settings.coverage[1] = value - settings.coverage[0];
        });
    });
    connect(ratio_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        edit([&](auto& settings) {
            const double total = settings.coverage[0] + settings.coverage[1];
            settings.coverage[0] = total * value;
            settings.coverage[1] = total - settings.coverage[0];
        });
    });
    build_thread(0, layout);
    build_thread(1, layout);
    notice_ = new QLabel(this);
    notice_->setObjectName("cloth_material_notice");
    notice_->setTextFormat(Qt::PlainText);
    notice_->setWordWrap(true);
    layout->addRow(notice_);
    hide();
}

void ClothMaterialPanel::build_thread(std::size_t index, QFormLayout* layout) {
    const QString prefix = index == 0 ? "cloth_warp" : "cloth_weft";
    auto* group = new QGroupBox(index == 0 ? "经向纱线" : "纬向纱线", this);
    auto* fields = form(group);
    fields->setContentsMargins(8, 12, 8, 12);
    auto& controls = threads_[index];
    auto* color = new QWidget(group);
    auto* components = new QHBoxLayout(color);
    components->setContentsMargins(0, 0, 0, 0);
    components->setSpacing(6);
    for (std::size_t channel = 0; channel < 3; ++channel) {
        controls.color[channel] =
            scalar(color, prefix + ".color." + QString::number(channel), 0, 1, .01);
        // 明确使用 RGB 标签，控件名称保留通道索引以便回归定位。
        controls.color[channel]->setPrefix(
            QString("%1 ").arg(QStringList{"R", "G", "B"}[int(channel)]));
        components->addWidget(controls.color[channel], 1);
        connect(
            controls.color[channel], &QDoubleSpinBox::valueChanged, this,
            [this, index, channel](double value) {
                edit([&](auto& settings) { settings.threads[index].albedo[int(channel)] = value; });
            });
    }
    controls.kd = scalar(group, prefix + ".kd", 0, 1, .05);
    controls.surface = scalar(group, prefix + ".surface_width", 1, 90, .5);
    controls.volume = scalar(group, prefix + ".volume_width", 1, 90, .5);
    fields->addRow("颜色 RGB", color);
    fields->addRow("各向同性比例", controls.kd);
    fields->addRow("表面宽度 °", controls.surface);
    fields->addRow("内部宽度 °", controls.volume);
    connect(controls.kd, &QDoubleSpinBox::valueChanged, this, [this, index](double value) {
        edit([&](auto& settings) { settings.threads[index].kd = value; });
    });
    connect(controls.surface, &QDoubleSpinBox::valueChanged, this, [this, index](double value) {
        edit([&](auto& settings) { settings.threads[index].surface_width_degrees = value; });
    });
    connect(controls.volume, &QDoubleSpinBox::valueChanged, this, [this, index](double value) {
        edit([&](auto& settings) { settings.threads[index].volume_width_degrees = value; });
    });
    controls.angles = new QLineEdit(group);
    controls.lengths = new QLineEdit(group);
    controls.angles->setObjectName(prefix + ".angles");
    controls.lengths->setObjectName(prefix + ".lengths");
    controls.angles->setMaxLength(1024);
    controls.lengths->setMaxLength(1024);
    fields->addRow("切向角 °", controls.angles);
    fields->addRow("分段长度", controls.lengths);
    auto* apply = new QPushButton("应用曲线", group);
    apply->setObjectName(prefix + ".apply_curve");
    fields->addRow(apply);
    connect(apply, &QPushButton::clicked, this, [this, index] { apply_curve(index); });
    layout->addRow(group);
}

void ClothMaterialPanel::set_material(const minirender::Material* material,
                                      std::uint64_t scene_generation) {
    const bool enabled = material && material->kind == minirender::MaterialKind::cloth;
    setVisible(enabled);
    if (!enabled) {
        appearance_.reset();
        material_id_.clear();
        notice_->clear();
        return;
    }
    if (material_id_ != material->id || generation_ != scene_generation) {
        notice_->clear();
        for (auto& fields : threads_) {
            fields.angles->setModified(false);
            fields.lengths->setModified(false);
        }
    }
    material_id_ = material->id;
    generation_ = scene_generation;
    appearance_ = material->cloth;
    refresh();
}

void ClothMaterialPanel::refresh() {
    if (!appearance_) {
        return;
    }
    refreshing_ = true;
    const auto& settings = appearance_->settings();
    rotation_->setValue(settings.rotation_degrees);
    correlation_->setValue(settings.correlation_width_degrees);
    samples_->setValue(int(settings.samples_per_thread));
    const double total = settings.coverage[0] + settings.coverage[1];
    coverage_->setValue(total);
    ratio_->setValue(settings.coverage[0] / total);
    for (std::size_t i = 0; i < 2; ++i) {
        auto& controls = threads_[i];
        const auto& thread = settings.threads[i];
        for (int channel = 0; channel < 3; ++channel) {
            controls.color[std::size_t(channel)]->setValue(thread.albedo[channel]);
        }
        controls.kd->setValue(thread.kd);
        controls.surface->setValue(thread.surface_width_degrees);
        controls.volume->setValue(thread.volume_width_degrees);
        if (!controls.angles->isModified()) {
            controls.angles->setText(curve_text(thread.curve.angles));
        }
        if (!controls.lengths->isModified()) {
            controls.lengths->setText(curve_text(thread.curve.lengths));
        }
    }
    refreshing_ = false;
}

void ClothMaterialPanel::edit(
    const std::function<void(minirender::ClothAppearanceSettings&)>& work) {
    if (refreshing_ || !appearance_) {
        return;
    }
    try {
        auto settings = appearance_->settings();
        work(settings);
        const auto next = std::make_shared<const minirender::ClothAppearance>(std::move(settings));
        if (on_changed) {
            on_changed(next, std::nullopt);
        }
        notice_->clear();
    } catch (const std::exception& error) {
        refresh();
        notice_->setText(QString::fromUtf8(error.what()));
    }
}

void ClothMaterialPanel::apply_curve(std::size_t index) {
    if (!appearance_) {
        return;
    }
    try {
        auto settings = appearance_->settings();
        settings.threads[index].curve = {curve_values(threads_[index].angles->text()),
                                         curve_values(threads_[index].lengths->text())};
        const auto next = std::make_shared<const minirender::ClothAppearance>(std::move(settings));
        threads_[index].angles->setModified(false);
        threads_[index].lengths->setModified(false);
        if (on_changed) {
            on_changed(next, std::nullopt);
        }
        notice_->clear();
    } catch (const std::exception& error) {
        notice_->setText(QString::fromUtf8(error.what()));
    }
}
