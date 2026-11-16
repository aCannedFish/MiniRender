// 工作台数值输入、三分量编辑和滚动表单的实现。

#include "workbench_internal.hpp"
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLocale>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <algorithm>
#include <array>

namespace workbench_detail {
std::filesystem::path native_path(const QString& path) {
    const auto bytes = path.toUtf8();
    return std::filesystem::path(std::u8string(bytes.begin(), bytes.end()));
}

// 省略数值末尾的零，输入范围与解析仍由 QDoubleSpinBox 管理。
class CompactScalar final : public QDoubleSpinBox {
  public:
    using QDoubleSpinBox::QDoubleSpinBox;

  protected:
    QString textFromValue(double value) const override {
        auto format = locale();
        format.setNumberOptions(format.numberOptions() | QLocale::OmitGroupSeparator);
        auto text = format.toString(value, 'f', decimals());
        if (decimals() > 0) {
            while (text.endsWith('0')) {
                text.chop(1);
            }
            if (text.endsWith(format.decimalPoint())) {
                text.chop(format.decimalPoint().size());
            }
        }
        return text;
    }
};

QDoubleSpinBox* spin(QWidget* parent, const QString& name, double minimum, double maximum,
                     double step) {
    auto* value = new CompactScalar(parent);
    value->setObjectName(name);
    value->setRange(minimum, maximum);
    value->setDecimals(4);
    value->setSingleStep(step);
    value->setKeyboardTracking(false);
    value->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return value;
}

QScrollArea* scroll_panel(QWidget* panel) {
    auto* area = new QScrollArea;
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    panel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    area->setWidget(panel);
    return area;
}

void configure_form(QFormLayout* form) {
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFormAlignment(Qt::AlignTop | Qt::AlignLeft);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(6);
    form->setContentsMargins(12, 8, 12, 12);
}

} // namespace workbench_detail

VectorField::VectorField(QWidget* parent, const QString& prefix, double minimum, double maximum)
    : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    // QFormLayout 根据整行宽度换行，避免分量采用 Ignored 策略时被压入标签旁。
    setMinimumWidth(3 * 88 + 2 * layout->spacing());
    const std::array<QString, 3> axes{"x", "y", "z"};
    for (std::size_t index = 0; index < values_.size(); ++index) {
        values_[index] =
            workbench_detail::spin(this, prefix + "." + axes[index], minimum, maximum, 0.1);
        const bool                   color = prefix == "base_color" || prefix == "emission";
        const std::array<QString, 3> channels{"r", "g", "b"};
        values_[index]->setPrefix((color ? channels[index] : axes[index]) + " ");
        if (color) {
            values_[index]->setToolTip("线性 RGB");
        }
        values_[index]->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        values_[index]->setMinimumWidth(88);
        values_[index]->setProperty("axis", axes[index]);
        layout->addWidget(values_[index], 1);
        connect(values_[index], &QDoubleSpinBox::valueChanged, this, [this] {
            if (on_changed) {
                on_changed();
            }
        });
    }
}

glm::vec3 VectorField::value() const {
    return {static_cast<float>(values_[0]->value()), static_cast<float>(values_[1]->value()),
            static_cast<float>(values_[2]->value())};
}

void VectorField::set_value(glm::vec3 value) {
    for (int index = 0; index < 3; ++index) {
        const QSignalBlocker blocker(values_[static_cast<std::size_t>(index)]);
        values_[static_cast<std::size_t>(index)]->setValue(value[index]);
    }
}
