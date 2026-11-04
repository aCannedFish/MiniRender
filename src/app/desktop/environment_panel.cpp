#include "environment_panel.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>

namespace {
QString stage_name(minirender::IblStage stage) {
    switch (stage) {
    case minirender::IblStage::source:
        return "检查环境图";
    case minirender::IblStage::mipmaps:
        return "生成环境 mipmap";
    case minirender::IblStage::diffuse:
        return "预计算漫反射";
    case minirender::IblStage::specular:
        return "预滤波镜面环境";
    case minirender::IblStage::brdf:
        return "预积分 BRDF";
    case minirender::IblStage::complete:
        return "整理预处理结果";
    }
    return {};
}
} // namespace
EnvironmentPanel::EnvironmentPanel(VulkanViewport& viewport, IblController& controller,
                                   QWidget* parent)
    : QWidget(parent) {
    setObjectName("environment_panel");
    auto* form = new QFormLayout(this);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    texture_ = new QComboBox(this);
    texture_->setObjectName("environment_texture");
    texture_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    texture_->setMinimumContentsLength(10);
    import_ = new QPushButton("导入环境图", this);
    import_->setObjectName("environment_import");
    const auto spin = [this](const char* name, double low, double high, double step) {
        auto* field = new QDoubleSpinBox(this);
        field->setObjectName(name);
        field->setRange(low, high);
        field->setDecimals(3);
        field->setSingleStep(step);
        field->setKeyboardTracking(false);
        connect(field, &QDoubleSpinBox::valueChanged, this, [this] { edit(); });
        return field;
    };
    auto* colors = new QWidget(this);
    auto* row = new QHBoxLayout(colors);
    row->setContentsMargins(0, 0, 0, 0);
    const std::array<const char*, 3> names{"environment_constant_r", "environment_constant_g",
                                           "environment_constant_b"};
    const std::array<QString, 3> prefixes{"R ", "G ", "B "};
    for (std::size_t i = 0; i < 3; ++i) {
        constant_[i] = spin(names[i], 0, 100, 0.05);
        constant_[i]->setPrefix(prefixes[i]);
        constant_[i]->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        constant_[i]->setMinimumWidth(75);
        row->addWidget(constant_[i], 1);
    }
    strength_ = spin("environment_strength", 0, 1000, 0.1);
    rotation_ = spin("environment_rotation", -360, 360, 5);
    form->addRow("常量环境 / RGB", colors);
    form->addRow("环境图", texture_);
    form->addRow(import_);
    form->addRow("图像强度", strength_);
    form->addRow("绕 +Y 旋转 / °", rotation_);
    auto* background = new QCheckBox("显示环境背景", this);
    background->setObjectName("environment_background");
    background->setChecked(viewport.environment_background());
    connect(background, &QCheckBox::toggled, this,
            [&viewport](bool enabled) { viewport.set_environment_background(enabled); });
    form->addRow(background);
    auto* cancel = new QPushButton("取消预处理", this);
    cancel->setObjectName("environment_cancel");
    auto* retry = new QPushButton("重试预处理", this);
    retry->setObjectName("environment_retry");
    form->addRow(cancel, retry);
    connect(cancel, &QPushButton::clicked, this, [&controller] { controller.cancel(); });
    connect(retry, &QPushButton::clicked, this, [&controller] { controller.retry(); });
    auto* status = new QLabel(this);
    status->setObjectName("environment_status");
    status->setWordWrap(true);
    status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* progress = new QProgressBar(this);
    progress->setObjectName("environment_progress");
    form->addRow(status);
    form->addRow(progress);
    auto* help = new QLabel("环境图按 latlong 解释，建议使用 2:1 "
                            "HDR/EXR。强度和旋转直接复用预处理结果。准备未完成时使用常量环境光。",
                            this);
    help->setWordWrap(true);
    form->addRow(help);
    connect(texture_, &QComboBox::currentIndexChanged, this, [this] { edit(); });
    connect(import_, &QPushButton::clicked, this, [this] {
        if (on_import) {
            on_import();
        }
    });
    auto* timer = new QTimer(this);
    timer->setInterval(100);
    connect(timer, &QTimer::timeout, this,
            [&controller, &viewport, status, progress, cancel, retry, background] {
                const auto s = controller.status();
                const bool busy =
                    s.state == IblTaskState::preparing || s.state == IblTaskState::cancelling;
                cancel->setEnabled(s.state == IblTaskState::preparing);
                retry->setEnabled(s.state == IblTaskState::failed ||
                                  s.state == IblTaskState::cancelled);
                background->setEnabled(!viewport.offline_visible());
                QString message;
                switch (s.state) {
                case IblTaskState::idle:
                    message = "等待场景";
                    break;
                case IblTaskState::preparing:
                    message =
                        s.waiting_for_previous ? "等待上一任务退出" : stage_name(s.progress.stage);
                    break;
                case IblTaskState::ready:
                    message = viewport.ibl_maps() ? "环境光就绪" : "常量环境光就绪";
                    break;
                case IblTaskState::cancelling:
                    message = "正在取消预处理";
                    break;
                case IblTaskState::cancelled:
                    message = "已取消，使用常量环境光";
                    break;
                case IblTaskState::failed:
                    message = "预处理失败：" + s.error;
                    break;
                }
                const bool known =
                    busy && s.progress.total > 0 && s.state != IblTaskState::cancelling;
                progress->setRange(0, busy && !known ? 0 : 10000);
                if (known) {
                    const double ratio = static_cast<double>(s.progress.completed) /
                                         static_cast<double>(s.progress.total);
                    progress->setValue(static_cast<int>(10000 * ratio));
                    message += QString("\n%1 / %2 个像素（%3%）")
                                   .arg(s.progress.completed)
                                   .arg(s.progress.total)
                                   .arg(100 * ratio, 0, 'f', 1);
                } else if (!busy) {
                    progress->setValue(s.state == IblTaskState::ready ? 10000 : 0);
                }
                status->setText(message);
            });
    timer->start();
}
void EnvironmentPanel::set_document(const minirender::SceneDocument& document) {
    const QSignalBlocker texture_block(texture_), strength_block(strength_),
        rotation_block(rotation_);
    texture_->clear();
    texture_->addItem("无", QString());
    for (const auto& texture : document.textures) {
        const auto id = QString::fromStdString(texture.id);
        const auto filename = QString::fromStdU16String(texture.source.filename().u16string());
        texture_->addItem(filename.isEmpty() ? id : filename + " (" + id + ")", id);
    }
    texture_->setCurrentIndex(
        texture_->findData(QString::fromStdString(document.environment_map.texture_id)));
    strength_->setValue(document.environment_map.strength);
    rotation_->setValue(document.environment_map.rotation_degrees);
    const bool mapped = !document.environment_map.texture_id.empty();
    strength_->setEnabled(mapped);
    rotation_->setEnabled(mapped);
    for (int i = 0; i < 3; ++i) {
        const QSignalBlocker blocker(constant_[static_cast<std::size_t>(i)]);
        constant_[static_cast<std::size_t>(i)]->setValue(document.environment[i]);
    }
}
void EnvironmentPanel::edit() {
    if (!on_edited) {
        return;
    }
    minirender::EnvironmentMap map;
    map.texture_id = texture_->currentData().toString().toStdString();
    map.strength = static_cast<float>(strength_->value());
    map.rotation_degrees = static_cast<float>(rotation_->value());
    const glm::vec3 constant{static_cast<float>(constant_[0]->value()),
                             static_cast<float>(constant_[1]->value()),
                             static_cast<float>(constant_[2]->value())};
    on_edited(constant, map);
}
void EnvironmentPanel::set_import_enabled(bool enabled) {
    import_->setEnabled(enabled);
}
