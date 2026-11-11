#include "volume_panel.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QTimer>

#include <algorithm>

namespace {
// 输入仍由原生 SpinBox 处理，显示时省略小数尾零，让三个并排字段保持可读。
class CompactScalar final : public QDoubleSpinBox {
  public:
    using QDoubleSpinBox::QDoubleSpinBox;

  protected:
    QString textFromValue(double number) const override {
        auto text = QLocale::c().toString(number, 'f', decimals());
        if (text.contains('.')) {
            while (text.endsWith('0')) {
                text.chop(1);
            }
            if (text.endsWith('.')) {
                text.chop(1);
            }
        }
        return text == "-0" ? "0" : text;
    }
};
QWidget* row(QWidget* parent, std::initializer_list<QWidget*> controls) {
    auto* widget = new QWidget(parent);
    auto* layout = new QHBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    for (auto* value : controls) {
        layout->addWidget(value, 1);
    }
    return widget;
}
void configure(QFormLayout& form) {
    form.setVerticalSpacing(12);
    form.setRowWrapPolicy(QFormLayout::WrapLongRows);
    form.setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
}
QDoubleSpinBox* scalar(QWidget* parent, const char* name, double low, double high, int decimals) {
    auto* spin = new CompactScalar(parent);
    spin->setObjectName(name);
    spin->setRange(low, high);
    spin->setDecimals(decimals);
    spin->setSingleStep(.05);
    spin->setKeyboardTracking(false);
    spin->setLocale(QLocale::c());
    spin->setMinimumWidth(82);
    spin->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    return spin;
}
void value(QDoubleSpinBox* spin, double number) {
    spin->setValue(number);
    spin->setToolTip("场景值：" +
                     QLocale::c().toString(number, 'g', QLocale::FloatingPointShortest));
}
} // namespace

VolumePanel::VolumePanel(QWidget* parent) : QWidget(parent) {
    setObjectName("volume_panel");
    auto* form = new QFormLayout(this);
    form->setContentsMargins(10, 12, 10, 12);
    configure(*form);
    demo_ = new QComboBox(this);
    demo_->setObjectName("volume_demo_kind");
    demo_->addItem("有界雾", "bounded_fog");
    demo_->addItem("Cornell Box · 雾", "cornell_fog");
    demo_->addItem("透光孔光束", "light_beam");
    const auto button = [&](const QString& text, const char* name,
                            std::function<void()> operation) {
        auto* result = new QPushButton(text, this);
        result->setObjectName(name);
        connect(result, &QPushButton::clicked, this,
                [this, operation = std::move(operation)] { invoke(operation); });
        return result;
    };
    load_ = button("载入示例", "volume_demo", [this] {
        if (on_demo) {
            on_demo(demo_->currentData().toString());
        }
    });
    load_->setToolTip("覆盖当前场景，导入后可继续编辑。");
    form->addRow(row(this, {demo_, load_}));
    auto* hint = new QLabel(
        "体积效果通过离线渲染显示。实时预览用于编辑表面；区域参数修改后，先应用再渲染。", this);
    hint->setWordWrap(true);
    hint->setTextFormat(Qt::PlainText);
    form->addRow(hint);
    render_ = button("开始离线渲染", "volume_render", [this] {
        if (on_render) {
            on_render();
        }
    });
    form->addRow(render_);
    regions_ = new QComboBox(this);
    regions_->setObjectName("volume_region");
    connect(regions_, &QComboBox::currentIndexChanged, this, [this] {
        if (!refreshing_) {
            select();
        }
    });
    form->addRow("介质区域", regions_);
    add_ = button("新增", "volume_add", [this] { add(); });
    remove_ = button("删除", "volume_remove", [this] { remove(); });
    form->addRow(row(this, {add_, remove_}));
    summary_ = new QLabel(this);
    summary_->setWordWrap(true);
    summary_->setTextFormat(Qt::PlainText);
    form->addRow(summary_);
    parameters_ = new QWidget(this);
    auto* parameters = new QFormLayout(parameters_);
    parameters->setContentsMargins(0, 0, 0, 0);
    configure(*parameters);
    name_ = new QLineEdit(parameters_);
    name_->setObjectName("volume_name");
    enabled_ = new QCheckBox("启用区域", parameters_);
    enabled_->setObjectName("volume_enabled");
    parameters->addRow("名称", name_);
    parameters->addRow(enabled_);
    connect(name_, &QLineEdit::textChanged, this, [this](const QString& name) {
        edit([&](auto& region) { region.name = name.toStdString(); });
    });
    connect(enabled_, &QCheckBox::toggled, this,
            [this](bool enabled) { edit([&](auto& region) { region.enabled = enabled; }); });
    parameters->addRow("最小坐标 / m",
                       vector_field(
                           "volume_min", minimum_,
                           [](auto& region) -> glm::dvec3& { return region.minimum; }, -1e9, 1e9,
                           false));
    parameters->addRow("最大坐标 / m",
                       vector_field(
                           "volume_max", maximum_,
                           [](auto& region) -> glm::dvec3& { return region.maximum; }, -1e9, 1e9,
                           false));
    parameters->addRow("吸收 / m⁻¹",
                       vector_field(
                           "volume_absorption", absorption_,
                           [](auto& region) -> glm::dvec3& { return region.medium.absorption; }, 0,
                           1e6, true));
    parameters->addRow("散射 / m⁻¹",
                       vector_field(
                           "volume_scattering", scattering_,
                           [](auto& region) -> glm::dvec3& { return region.medium.scattering; }, 0,
                           1e6, true));
    g_ = scalar(parameters_, "volume_g", -.999999, .999999, 6);
    g_->setToolTip("正值以前向散射为主，负值以后向散射为主，0 为各向同性。");
    parameters->addRow("各向异性 g", g_);
    connect(g_, &QDoubleSpinBox::valueChanged, this,
            [this](double g) { edit([&](auto& region) { region.medium.anisotropy = g; }); });
    form->addRow(parameters_);
    apply_ = button("应用区域", "volume_apply", [this] { apply(); });
    form->addRow(apply_);
    notice_ = new QLabel(this);
    notice_->setObjectName("volume_notice");
    notice_->setWordWrap(true);
    notice_->setTextFormat(Qt::PlainText);
    form->addRow(notice_);
    auto* timer = new QTimer(this);
    timer->setInterval(200);
    connect(timer, &QTimer::timeout, this, [this] { update_buttons(); });
    timer->start();
    update_buttons();
}

QWidget* VolumePanel::vector_field(const char* name, std::array<QDoubleSpinBox*, 3>& controls,
                                   VectorAccess access, double low, double high, bool rgb) {
    for (std::size_t i = 0; i < 3; ++i) {
        const auto id = std::string(name) + '.' + std::to_string(i);
        auto* spin = scalar(parameters_, id.c_str(), low, high, 9);
        controls[i] = spin;
        spin->setPrefix(
            (rgb ? QStringList{"R ", "G ", "B "} : QStringList{"X ", "Y ", "Z "})[int(i)]);
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this, access, i](double number) {
            edit([&](auto& region) { access(region)[int(i)] = number; });
        });
    }
    return row(parameters_, {controls[0], controls[1], controls[2]});
}

void VolumePanel::set_document(std::span<const minirender::MediumRegion> regions,
                               std::uint64_t generation) {
    if (initialized_ && generation_ == generation && source_.size() == regions.size() &&
        std::equal(source_.begin(), source_.end(), regions.begin())) {
        return;
    }
    initialized_ = true;
    generation_ = generation;
    source_.assign(regions.begin(), regions.end());
    refreshing_ = true;
    regions_->clear();
    for (const auto& region : source_) {
        regions_->addItem(QString::fromStdString(region.name.empty() ? region.id : region.name),
                          QString::fromStdString(region.id));
    }
    const int found = regions_->findData(QString::fromStdString(selected_));
    regions_->setCurrentIndex(found >= 0 ? found : source_.empty() ? -1 : 0);
    refreshing_ = false;
    select();
}
void VolumePanel::select() {
    selected_ = regions_->currentData().toString().toStdString();
    const auto found = std::find_if(source_.begin(), source_.end(),
                                    [&](const auto& region) { return region.id == selected_; });
    draft_ = found == source_.end() ? std::nullopt : std::optional(*found);
    dirty_ = false;
    notice_->clear();
    refresh_fields();
}
void VolumePanel::refresh_fields() {
    refreshing_ = true;
    if (draft_) {
        name_->setText(QString::fromStdString(draft_->name));
        enabled_->setChecked(draft_->enabled);
        for (std::size_t i = 0; i < 3; ++i) {
            value(minimum_[i], draft_->minimum[int(i)]);
            value(maximum_[i], draft_->maximum[int(i)]);
            value(absorption_[i], draft_->medium.absorption[int(i)]);
            value(scattering_[i], draft_->medium.scattering[int(i)]);
        }
        value(g_, draft_->medium.anisotropy);
        g_->setToolTip(g_->toolTip() + "\n正值以前向散射为主，负值以后向散射为主，0 为各向同性。");
    }
    refreshing_ = false;
    update_buttons();
}
void VolumePanel::edit(const std::function<void(minirender::MediumRegion&)>& operation) {
    if (refreshing_ || !draft_) {
        return;
    }
    operation(*draft_);
    const auto original = std::find_if(source_.begin(), source_.end(),
                                       [&](const auto& region) { return region.id == selected_; });
    dirty_ = original == source_.end() || *draft_ != *original;
    notice_->setText(dirty_ ? "草稿尚未应用。" : "");
    update_buttons();
}
void VolumePanel::set_asset_busy(bool busy) {
    busy_ = busy;
    update_buttons();
}
void VolumePanel::update_buttons() {
    parameters_->setEnabled(draft_.has_value() && !busy_);
    add_->setEnabled(!busy_ && source_.size() < minirender::maximum_medium_regions);
    remove_->setEnabled(draft_.has_value() && !busy_);
    apply_->setEnabled(draft_.has_value() && dirty_ && !busy_);
    load_->setEnabled(!busy_);
    const bool active = render_active && render_active();
    render_->setEnabled(!busy_ && (!dirty_ || active));
    render_->setText(active ? "查看渲染任务" : "开始离线渲染");
    summary_->setText(QString("%1 / %2 个区域；启用区域的内部不能重叠。")
                          .arg(source_.size())
                          .arg(minirender::maximum_medium_regions));
}
void VolumePanel::invoke(const std::function<void()>& operation) {
    try {
        operation();
    } catch (const std::exception& error) {
        notice_->setText(QString::fromUtf8(error.what()));
    }
}
void VolumePanel::publish(std::vector<minirender::MediumRegion> regions, std::string selected) {
    minirender::validate_medium_regions(regions);
    if (!on_changed) {
        return;
    }
    const auto previous = selected_;
    selected_ = std::move(selected);
    try {
        on_changed(std::move(regions));
    } catch (...) {
        selected_ = previous;
        throw;
    }
}
void VolumePanel::apply() {
    if (!draft_) {
        return;
    }
    auto regions = source_;
    const auto found = std::find_if(regions.begin(), regions.end(),
                                    [&](const auto& region) { return region.id == selected_; });
    if (found == regions.end()) {
        return;
    }
    *found = *draft_;
    publish(std::move(regions), selected_);
    notice_->setText("区域已应用，开始离线渲染可查看结果。");
}
void VolumePanel::add() {
    auto regions = source_;
    minirender::MediumRegion region;
    for (std::size_t i = 1;; ++i) {
        region.id = "medium_" + std::to_string(i);
        if (std::none_of(regions.begin(), regions.end(),
                         [&](const auto& value) { return value.id == region.id; })) {
            break;
        }
    }
    region.name = "介质区域";
    region.medium.absorption = glm::dvec3(.05);
    region.medium.scattering = glm::dvec3(.25);
    if (!regions.empty()) {
        double right = regions.front().maximum.x;
        for (const auto& value : regions) {
            right = std::max(right, value.maximum.x);
        }
        region.minimum.x = right + .25;
        region.maximum.x = right + 2.25;
    }
    const auto id = region.id;
    regions.push_back(std::move(region));
    publish(std::move(regions), id);
}
void VolumePanel::remove() {
    auto regions = source_;
    std::erase_if(regions, [&](const auto& region) { return region.id == selected_; });
    publish(std::move(regions), {});
}
