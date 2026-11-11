#include "cloth_panel.hpp"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>

#include <algorithm>

namespace {
QDoubleSpinBox* scalar(QWidget* parent, const char* name, double low, double high, double step = .1,
                       int decimals = 5) {
    auto* value = new QDoubleSpinBox(parent);
    value->setObjectName(name);
    value->setRange(low, high);
    value->setSingleStep(step);
    value->setDecimals(decimals);
    value->setKeyboardTracking(false);
    value->setLocale(QLocale::c());
    value->setMinimumWidth(82);
    value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    return value;
}
QSpinBox* integer(QWidget* parent, const char* name, int low, int high) {
    auto* value = new QSpinBox(parent);
    value->setObjectName(name);
    value->setRange(low, high);
    value->setKeyboardTracking(false);
    return value;
}
QWidget* row(QWidget* parent, std::initializer_list<QWidget*> values) {
    auto* widget = new QWidget(parent);
    auto* layout = new QHBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    for (auto* value : values) {
        layout->addWidget(value, 1);
    }
    return widget;
}
} // namespace

ClothPanel::ClothPanel(ClothController& controller, QWidget* parent)
    : QWidget(parent), controller_(controller) {
    setObjectName("cloth_panel");
    auto* layout = new QFormLayout(this);
    layout->setContentsMargins(10, 12, 10, 12);
    layout->setVerticalSpacing(12);
    layout->setRowWrapPolicy(QFormLayout::WrapLongRows);
    layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    demo_ = new QComboBox(this);
    demo_->setObjectName("cloth_demo_kind");
    demo_->addItems({"落球", "悬挂迎风"});
    auto* demo = new QPushButton("载入示例", this);
    demo->setObjectName("cloth_demo");
    demo->setToolTip("覆盖当前场景，创建可编辑的布料与碰撞体。");
    connect(demo, &QPushButton::clicked, this, [this] {
        if (on_demo) {
            on_demo(demo_->currentIndex());
        }
    });
    layout->addRow(row(this, {demo_, demo}));
    target_ = new QComboBox(this);
    target_->setObjectName("cloth_target");
    connect(target_, &QComboBox::currentIndexChanged, this, [this] {
        if (!refreshing_ && on_target) {
            on_target(target_->currentData().toString().toStdString());
        }
    });
    layout->addRow("模拟物体", target_);
    const auto button = [&](const QString& text, const char* name,
                            const std::function<void()>& work) {
        auto* value = new QPushButton(text, this);
        value->setObjectName(name);
        connect(value, &QPushButton::clicked, this, work);
        return value;
    };
    const auto preview = [this] {
        if (on_preview) {
            on_preview();
        }
    };
    play_ = button("播放", "cloth_play", [this, preview] {
        preview();
        controller_.play();
    });
    pause_ = button("暂停", "cloth_pause", [this] { controller_.pause(); });
    step_ = button("单步", "cloth_step", [this, preview] {
        preview();
        controller_.step();
    });
    reset_ = button("重置", "cloth_reset", [this, preview] {
        preview();
        controller_.reset();
    });
    freeze_ = button("冻结为网格", "cloth_freeze", [this] {
        if (on_freeze) {
            on_freeze();
        }
    });
    layout->addRow(row(this, {play_, pause_, step_}));
    layout->addRow(row(this, {reset_, freeze_}));
    auto* hint = new QLabel(
        "保存保留当前帧及模拟参数。冻结后可作为普通网格继续编辑；离线渲染会先暂停并固定当前状态。",
        this);
    hint->setWordWrap(true);
    hint->setTextFormat(Qt::PlainText);
    layout->addRow(hint);
    status_ = new QLabel(this);
    details_ = new QLabel(this);
    notice_ = new QLabel(this);
    status_->setObjectName("cloth_status");
    details_->setObjectName("cloth_details");
    notice_->setObjectName("cloth_notice");
    for (auto* value : {status_, details_, notice_}) {
        value->setWordWrap(true);
        value->setTextFormat(Qt::PlainText);
    }
    progress_ = new QProgressBar(this);
    progress_->setObjectName("cloth_progress");
    layout->addRow(status_);
    layout->addRow(details_);
    layout->addRow(progress_);
    columns_ = integer(this, "cloth_columns", 2, 256);
    rows_ = integer(this, "cloth_rows", 2, 256);
    width_ = scalar(this, "cloth_width", .01, 1000);
    height_ = scalar(this, "cloth_height", .01, 1000);
    pins_ = new QComboBox(this);
    pins_->setObjectName("cloth_pins");
    pins_->addItems({"不固定", "上方两角", "上边整行", "四角", "自定义（保留）"});
    layout->addRow("列数", columns_);
    layout->addRow("行数", rows_);
    layout->addRow("宽度 / m", width_);
    layout->addRow("高度 / m", height_);
    layout->addRow("固定点", pins_);
    const auto geometry = [this] {
        if (!refreshing_) {
            geometry_dirty_ = true;
        }
    };
    connect(columns_, &QSpinBox::valueChanged, this, geometry);
    connect(rows_, &QSpinBox::valueChanged, this, geometry);
    connect(width_, &QDoubleSpinBox::valueChanged, this, geometry);
    connect(height_, &QDoubleSpinBox::valueChanged, this, geometry);
    connect(pins_, &QComboBox::currentIndexChanged, this, geometry);
    density_ = scalar(this, "cloth_density", 1e-6, 1e4, .01, 6);
    structural_ = scalar(this, "cloth_structural", 0, 1e8, 10);
    shear_ = scalar(this, "cloth_shear", 0, 1e8, 10);
    bending_ = scalar(this, "cloth_bending", 0, 1e8, 1);
    spring_damping_ = scalar(this, "cloth_spring_damping", 0, 1, .01);
    velocity_damping_ = scalar(this, "cloth_velocity_damping", 0, 100, .05);
    thickness_ = scalar(this, "cloth_thickness", 0, 1, .001, 6);
    substeps_ = integer(this, "cloth_substeps", 1, 4096);
    layout->addRow("面密度 kg/m²", density_);
    layout->addRow("结构刚度 N/m", structural_);
    layout->addRow("剪切刚度 N/m", shear_);
    layout->addRow("弯曲刚度 N/m", bending_);
    layout->addRow("弹簧阻尼比", spring_damping_);
    layout->addRow("速度阻尼 s⁻¹", velocity_damping_);
    layout->addRow("厚度 / m", thickness_);
    layout->addRow("子步下限", substeps_);
    substeps_->setToolTip("按世界尺寸计算所需子步，实际固定数量取它与输入下限的较大值。");
    const auto bind_parameter = [this](QDoubleSpinBox* spin,
                                       double minirender::ClothParameters::* member) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this, member](double value) {
            if (!refreshing_) {
                draft_.parameters.*member = value;
            }
        });
    };
    bind_parameter(density_, &minirender::ClothParameters::surface_density);
    bind_parameter(structural_, &minirender::ClothParameters::structural_stiffness);
    bind_parameter(shear_, &minirender::ClothParameters::shear_stiffness);
    bind_parameter(bending_, &minirender::ClothParameters::bending_stiffness);
    bind_parameter(spring_damping_, &minirender::ClothParameters::spring_damping_ratio);
    bind_parameter(velocity_damping_, &minirender::ClothParameters::velocity_damping);
    bind_parameter(thickness_, &minirender::ClothParameters::thickness);
    connect(substeps_, &QSpinBox::valueChanged, this, [this](int value) {
        if (!refreshing_) {
            draft_.parameters.substeps = std::uint32_t(value);
        }
    });
    const auto vector = [&](const char* prefix, std::array<QDoubleSpinBox*, 3>& values,
                            glm::dvec3 minirender::ClothEnvironment::* member) {
        for (int i = 0; i < 3; ++i) {
            const auto name = std::string(prefix) + '.' + std::to_string(i);
            values[std::size_t(i)] = scalar(this, name.c_str(), -1000, 1000);
            values[std::size_t(i)]->setPrefix(QStringList{"X ", "Y ", "Z "}[i]);
            connect(values[std::size_t(i)], &QDoubleSpinBox::valueChanged, this,
                    [this, member, i](double value) {
                        if (!refreshing_) {
                            (draft_.environment.*member)[i] = value;
                        }
                    });
        }
        return row(this, {values[0], values[1], values[2]});
    };
    layout->addRow("重力 m/s²",
                   vector("cloth_gravity", gravity_, &minirender::ClothEnvironment::gravity));
    layout->addRow("风速 m/s",
                   vector("cloth_wind", wind_, &minirender::ClothEnvironment::wind_velocity));
    air_ = scalar(this, "cloth_air_density", 0, 100, .1);
    drag_ = scalar(this, "cloth_drag", 0, 100, .1);
    layout->addRow("空气密度 kg/m³", air_);
    layout->addRow("风阻系数", drag_);
    connect(air_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        if (!refreshing_) {
            draft_.environment.air_density = value;
        }
    });
    connect(drag_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        if (!refreshing_) {
            draft_.environment.drag_coefficient = value;
        }
    });
    apply_ = button("应用物理参数", "cloth_apply", [this] { apply(); });
    layout->addRow(apply_);
    layout->addRow(notice_);
    auto* timer = new QTimer(this);
    timer->setInterval(50);
    connect(timer, &QTimer::timeout, this, [this] { poll(); });
    timer->start();
    poll();
}

void ClothPanel::set_document(const minirender::SceneDocument& scene, const std::string& target) {
    refreshing_ = true;
    target_->clear();
    std::shared_ptr<const minirender::ClothDefinition> definition;
    for (const auto& object : scene.objects) {
        if (object.cloth) {
            target_->addItem(QString::fromStdString(object.name),
                             QString::fromStdString(object.id));
            if (object.id == target) {
                definition = object.cloth->definition();
            }
        }
    }
    target_->setCurrentIndex(target_->findData(QString::fromStdString(target)));
    if (definition != source_) {
        source_ = std::move(definition);
        if (source_) {
            draft_ = *source_;
            refresh_parameters();
        }
        notice_->clear();
    }
    refreshing_ = false;
    poll();
}

void ClothPanel::refresh_parameters() {
    const auto& grid = draft_.grid;
    columns_->setValue(int(grid.columns));
    rows_->setValue(int(grid.rows));
    width_->setValue(glm::length(grid.span_u));
    height_->setValue(glm::length(grid.span_v));
    const std::vector<std::uint32_t> corners{0, grid.columns - 1, (grid.rows - 1) * grid.columns,
                                             grid.columns * grid.rows - 1};
    std::vector<std::uint32_t> top;
    for (std::uint32_t i = 0; i < grid.columns; ++i) {
        top.push_back(i);
    }
    pins_->setCurrentIndex(grid.pinned.empty()                                              ? 0
                           : grid.pinned == std::vector<std::uint32_t>{0, grid.columns - 1} ? 1
                           : grid.pinned == top                                             ? 2
                           : grid.pinned == corners                                         ? 3
                                                                                            : 4);
    const auto& p = draft_.parameters;
    density_->setValue(p.surface_density);
    structural_->setValue(p.structural_stiffness);
    shear_->setValue(p.shear_stiffness);
    bending_->setValue(p.bending_stiffness);
    spring_damping_->setValue(p.spring_damping_ratio);
    velocity_damping_->setValue(p.velocity_damping);
    thickness_->setValue(p.thickness);
    substeps_->setValue(int(p.substeps));
    for (int i = 0; i < 3; ++i) {
        gravity_[std::size_t(i)]->setValue(draft_.environment.gravity[i]);
        wind_[std::size_t(i)]->setValue(draft_.environment.wind_velocity[i]);
    }
    air_->setValue(draft_.environment.air_density);
    drag_->setValue(draft_.environment.drag_coefficient);
    geometry_dirty_ = false;
}

void ClothPanel::apply() {
    if (!source_ || !on_apply) {
        return;
    }
    try {
        if (geometry_dirty_) {
            auto& grid = draft_.grid;
            const auto center = grid.origin + .5 * (grid.span_u + grid.span_v);
            grid.span_u = glm::normalize(grid.span_u) * width_->value();
            grid.span_v = glm::normalize(grid.span_v) * height_->value();
            grid.origin = center - .5 * (grid.span_u + grid.span_v);
            grid.columns = std::uint32_t(columns_->value());
            grid.rows = std::uint32_t(rows_->value());
            if (pins_->currentIndex() != 4) {
                grid.pinned.clear();
                if (pins_->currentIndex() == 1) {
                    grid.pinned = {0, grid.columns - 1};
                }
                if (pins_->currentIndex() == 2) {
                    for (std::uint32_t i = 0; i < grid.columns; ++i) {
                        grid.pinned.push_back(i);
                    }
                }
                if (pins_->currentIndex() == 3) {
                    grid.pinned = {0, grid.columns - 1, (grid.rows - 1) * grid.columns,
                                   grid.rows * grid.columns - 1};
                }
            }
        }
        draft_.validate();
        on_apply(std::make_shared<const minirender::ClothDefinition>(draft_), geometry_dirty_);
        notice_->clear();
    } catch (const std::exception& error) {
        notice_->setText(QString::fromUtf8(error.what()));
    }
}

void ClothPanel::poll() {
    const auto state = controller_.status();
    play_->setEnabled(state.bound && !state.playing && !state.failed);
    pause_->setEnabled(state.playing || state.busy);
    step_->setEnabled(state.bound && !state.playing && !state.busy);
    reset_->setEnabled(state.bound);
    freeze_->setEnabled(state.bound);
    apply_->setEnabled(state.bound);
    status_->setText(state.message);
    details_->setText(QString("帧 %1 | 时间 %2 s | 实际子步 %3\n长度比 %4 | 接触 %5 | 计算 %6 ms")
                          .arg(state.frame)
                          .arg(state.statistics.time_seconds, 0, 'f', 3)
                          .arg(state.effective_substeps)
                          .arg(state.statistics.maximum_spring_ratio, 0, 'f', 4)
                          .arg(state.last_steps.contacts)
                          .arg(state.last_work_ms, 0, 'f', 2));
    progress_->setVisible(state.busy);
    if (state.busy) {
        progress_->setFormat(state.stage + " %p%");
        if (state.total == 0) {
            progress_->setRange(0, 0);
        } else {
            progress_->setRange(0, 1000);
            progress_->setValue(int(1000.0 * double(state.completed) / double(state.total)));
        }
    }
}
