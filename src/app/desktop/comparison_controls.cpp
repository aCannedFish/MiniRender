// 构建实时显示比较控件，并按同一时刻的相机导出显示图与线性 HDR。
#include "comparison_controls.hpp"
#include "editor_theme.hpp"
#include "workbench_internal.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>

void add_comparison_controls(QFormLayout& layout, QComboBox& mode, VulkanViewport& viewport) {
    auto* panel = layout.parentWidget();
    auto* right = new QComboBox(panel);
    right->setObjectName("compare_right");
    right->addItem("关闭");
    for (int i = 0; i < mode.count(); ++i) {
        right->addItem(mode.itemText(i));
    }
    right->setToolTip("左侧使用当前显示模式。按住视口分界线拖动可调整两侧宽度。");
    layout.addRow("右侧对照", right);
    auto* reference = new QComboBox(panel);
    reference->addItems({"沿用左侧阴影", "硬阴影", "PCF", "PCSS"});
    reference->setObjectName("reference_shadow");
    reference->setEnabled(false);
    layout.addRow("右侧阴影", reference);
    QObject::connect(right, &QComboBox::currentIndexChanged, reference,
                     [reference](int i) { reference->setEnabled(i > 0); });
    QObject::connect(reference, &QComboBox::currentIndexChanged, panel,
                     [&viewport](int i) { viewport.set_reference_shadow(i - 1); });
    auto* energy = new QComboBox(panel);
    energy->setObjectName("reference_energy");
    energy->addItems({"沿用材质设置", "单次微面散射", "多次散射补偿"});
    energy->setEnabled(false);
    layout.addRow("右侧金属", energy);
    QObject::connect(right, &QComboBox::currentIndexChanged, energy,
                     [energy](int i) { energy->setEnabled(i > 0); });
    QObject::connect(energy, &QComboBox::currentIndexChanged, panel,
                     [&viewport](int i) { viewport.set_reference_energy(i - 1); });
    auto* save = new QPushButton("导出 A/B 图像…");
    save->setEnabled(false);
    layout.addRow(save);
    auto* info = new QLabel;
    info->setWordWrap(true);
    info->hide();
    layout.addRow(info);
    QObject::connect(right, &QComboBox::currentIndexChanged, panel, [&viewport, save](int i) {
        viewport.set_comparison_mode(i - 1);
        save->setEnabled(i > 0);
    });
    QObject::connect(save, &QPushButton::clicked, panel, [&viewport, panel, save, right, info] {
        if (viewport.offline_visible() || viewport.sdf_visible() ||
            viewport.point_preview_visible()) {
            info->setText("请切回实时场景后导出对照");
            info->show();
            return;
        }
        const auto dir = QFileDialog::getExistingDirectory(panel, "选择 A/B 图像的保存目录");
        if (dir.isEmpty()) {
            return;
        }
        try {
            // 读回发生在同一 GUI 回调内，键鼠事件无法在两次采集之间移动相机。
            const auto a = viewport.grab_comparison_side(0), b = viewport.grab_comparison_side(1);
            auto raw_a = viewport.grab_comparison_hdr(0), raw_b = viewport.grab_comparison_hdr(1);
            save->setEnabled(false);
            info->show();
            info->setText("正在写出 PNG 和 EXR…");
            workbench_detail::run_background(
                panel,
                [dir, a, b, raw_a = std::move(raw_a), raw_b = std::move(raw_b)] {
                    QTemporaryDir folder(dir + "/MiniRender-AB-XXXXXX");
                    if (!folder.isValid() || !a.save(folder.filePath("A.png")) ||
                        !b.save(folder.filePath("B.png"))) {
                        throw std::runtime_error("无法写出对照图像");
                    }
                    raw_a.write_exr(workbench_detail::native_path(folder.filePath("A.exr")));
                    raw_b.write_exr(workbench_detail::native_path(folder.filePath("B.exr")));
                    folder.setAutoRemove(false);
                    return folder.path();
                },
                [save, right, info](const auto& result) {
                    save->setEnabled(right->currentIndex() > 0);
                    info->setText(result.error.empty() ? "已保存到 " + result.value
                                                       : QString::fromStdString(result.error));
                });
        } catch (const std::exception& error) {
            info->setText(QString::fromUtf8(error.what()));
            info->show();
        }
    });
    // 教学参数独立于真实场景的材质和灯光，仅在任一侧选用教学着色时显示。
    auto* section  = new QWidget(panel);
    auto* settings = new QFormLayout(section);
    workbench_detail::configure_form(settings);
    auto* position = new VectorField(section, "teaching_position");
    position->set_value(glm::vec3(viewport.teaching_light()));
    settings->addRow("点光源 / m", position);
    auto* intensity = workbench_detail::spin(section, "teaching_intensity", 0, 100000, 0.5);
    intensity->setValue(viewport.teaching_light().w);
    settings->addRow("点光源强度", intensity);
    auto* exponent = workbench_detail::spin(section, "teaching_exponent", 1, 1024, 1);
    exponent->setValue(viewport.teaching_surface().x);
    settings->addRow("高光指数", exponent);
    auto* specular = workbench_detail::spin(section, "teaching_specular", 0, 1, .05);
    specular->setValue(viewport.teaching_surface().y);
    settings->addRow("镜面系数", specular);
    auto* textured = new QCheckBox("使用底色纹理");
    textured->setChecked(true);
    settings->addRow(textured);
    layout.addRow(section);
    const auto update = [=, &viewport] {
        viewport.set_teaching(glm::vec4(position->value(), static_cast<float>(intensity->value())),
                              {static_cast<float>(exponent->value()),
                               static_cast<float>(specular->value()),
                               textured->isChecked() ? 1.F : 0.F});
    };
    position->on_changed = update;
    for (auto* spin : {intensity, exponent, specular}) {
        QObject::connect(spin, &QDoubleSpinBox::valueChanged, section, update);
    }
    QObject::connect(textured, &QCheckBox::toggled, section, update);
    const auto visibility = [=, &mode] {
        section->setVisible(mode.currentIndex() == VulkanViewport::blinn_phong_mode ||
                            right->currentIndex() - 1 == VulkanViewport::blinn_phong_mode);
    };
    QObject::connect(&mode, &QComboBox::currentIndexChanged, section, visibility);
    QObject::connect(right, &QComboBox::currentIndexChanged, section, visibility);
    visibility();
}
