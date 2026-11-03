#include "realtime_panel.hpp"
#include "vulkan_viewport.hpp"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QSpinBox>

RealtimePanel::RealtimePanel(VulkanViewport& viewport, QWidget* parent) : QWidget(parent) {
    setObjectName("realtime_panel");
    auto* layout = new QFormLayout(this);
    layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    layout->setRowWrapPolicy(QFormLayout::WrapLongRows);
    auto* enabled = new QCheckBox("启用 SSAO", this);
    enabled->setObjectName("ssao_enabled");
    auto* filtered = new QCheckBox("使用双边滤波", this);
    filtered->setObjectName("ssao_filtered");
    filtered->setChecked(true);
    const auto value = [&](const char* name, double low, double high, double initial, double step) {
        auto* spin = new QDoubleSpinBox(this);
        spin->setObjectName(name);
        spin->setRange(low, high);
        spin->setDecimals(3);
        spin->setSingleStep(step);
        spin->setKeyboardTracking(false);
        spin->setValue(initial);
        return spin;
    };
    const auto defaults = viewport.ssao_settings();
    enabled->setChecked(defaults.enabled);
    filtered->setChecked(defaults.filtered);
    auto* radius = value("ssao_radius", 0.001, 1000, defaults.radius, 0.05);
    auto* bias = value("ssao_bias", 0, defaults.radius, defaults.bias, 0.005);
    auto* strength = value("ssao_strength", 0, 4, defaults.strength, 0.1);
    auto* samples = new QSpinBox(this);
    samples->setObjectName("ssao_samples");
    samples->setRange(1, 64);
    samples->setKeyboardTracking(false);
    samples->setValue(static_cast<int>(defaults.samples));
    layout->addRow(enabled);
    layout->addRow(filtered);
    layout->addRow("半径 / m", radius);
    layout->addRow("深度偏移 / m", bias);
    layout->addRow("强度", strength);
    layout->addRow("样本数", samples);
    auto* help = new QLabel(
        "SSAO 仅影响环境漫反射。相机页的显示列表可查看原始 AO 和滤波 AO。参数保留在当前视口中。",
        this);
    help->setWordWrap(true);
    layout->addRow(help);
    const auto update = [=, &viewport] {
        const QSignalBlocker blocker(bias);
        bias->setMaximum(radius->value());
        minirender::SsaoSettings settings;
        settings.enabled = enabled->isChecked();
        settings.filtered = filtered->isChecked();
        settings.radius = static_cast<float>(radius->value());
        settings.bias = static_cast<float>(bias->value());
        settings.strength = static_cast<float>(strength->value());
        settings.samples = static_cast<std::uint32_t>(samples->value());
        viewport.set_ssao_settings(settings);
    };
    connect(enabled, &QCheckBox::toggled, this, update);
    connect(filtered, &QCheckBox::toggled, this, update);
    for (auto* spin : {radius, bias, strength}) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, update);
    }
    connect(samples, &QSpinBox::valueChanged, this, update);
}
