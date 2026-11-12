#include "lightfield_panel.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <tuple>

namespace {
using namespace minirender;
QSpinBox* integer(QWidget* parent, const char* name, int low, int high, int value) {
    auto* s = new QSpinBox(parent);
    s->setObjectName(name);
    s->setRange(low, high);
    s->setValue(value);
    s->setKeyboardTracking(false);
    return s;
}
class OpticalScalar final : public QDoubleSpinBox {
  public:
    using QDoubleSpinBox::QDoubleSpinBox;

  protected:
    QString textFromValue(double v) const override {
        return locale().toString(v, 'g', QLocale::FloatingPointShortest);
    }
};
QDoubleSpinBox* scalar(QWidget* parent, const char* name, double low, double high, double value) {
    auto* s = new OpticalScalar(parent);
    s->setObjectName(name);
    s->setDecimals(323);
    s->setLocale(QLocale::c());
    s->setRange(low, high);
    s->setValue(value);
    s->setSingleStep(.01);
    s->setKeyboardTracking(false);
    return s;
}
QPushButton* button(QWidget* parent, const char* name, const QString& label) {
    auto* b = new QPushButton(label, parent);
    b->setObjectName(name);
    return b;
}
QFormLayout* form(QWidget* widget) {
    auto* f = new QFormLayout(widget);
    f->setContentsMargins(0, 0, 0, 0);
    f->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    f->setRowWrapPolicy(QFormLayout::WrapLongRows);
    return f;
}
QSlider* slider(QWidget* parent, const char* name) {
    auto* s = new QSlider(Qt::Horizontal, parent);
    s->setObjectName(name);
    s->setRange(0, 1000);
    return s;
}
QString phase(LightFieldPhase p) {
    switch (p) {
    case LightFieldPhase::preparing:
        return "准备场景";
    case LightFieldPhase::rendering:
        return "采集子视图";
    case LightFieldPhase::writing:
        return "写入子视图";
    case LightFieldPhase::finalizing:
        return "整理文件";
    }
    return {};
}
} // namespace
LightFieldPanel::LightFieldPanel(LightFieldController& controller, VulkanViewport& viewport,
                                 QWidget* parent)
    : QWidget(parent), controller_(controller), viewport_(viewport) {
    setObjectName("lf_panel");
    auto* layout = new QVBoxLayout(this);
    demo_ = button(this, "lf_demo", "导入近远棋盘");
    layout->addWidget(demo_);
    capture_fields_ = new QWidget(this);
    auto* f = form(capture_fields_);
    width_ = integer(this, "lf_width", 1, 16384, 128);
    height_ = integer(this, "lf_height", 1, 16384, 128);
    spp_ = integer(this, "lf_spp", 1, 1000000, 32);
    seed_ = new QLineEdit("101", this);
    seed_->setObjectName("lf_seed");
    columns_ = integer(this, "lf_columns", 3, 15, 5);
    rows_ = integer(this, "lf_rows", 3, 15, 5);
    columns_->setSingleStep(2);
    rows_->setSingleStep(2);
    reference_ = scalar(this, "lf_reference", 1e-6, 1e6, 4);
    radius_ = scalar(this, "lf_radius", 1e-6, 1e6, .04);
    threads_ = integer(this, "lf_threads", 0, 32, 0);
    threads_->setSpecialValueText("自动");
    memory_ = integer(this, "lf_memory", 1, 8192, 256);
    f->addRow("宽 / px", width_);
    f->addRow("高 / px", height_);
    f->addRow("每像素样本", spp_);
    f->addRow("随机种子", seed_);
    f->addRow("阵列列数", columns_);
    f->addRow("阵列行数", rows_);
    f->addRow("参考焦距 / m", reference_);
    f->addRow("采集半径 / m", radius_);
    f->addRow("工作线程", threads_);
    f->addRow("内存预算 / MiB", memory_);
    layout->addWidget(capture_fields_);
    auto* actions = new QGridLayout;
    start_ = button(this, "lf_start", "用 PT 采集当前场景");
    pause_ = button(this, "lf_pause", "暂停");
    cancel_ = button(this, "lf_cancel", "取消");
    open_ = button(this, "lf_open", "打开光场");
    save_ = button(this, "lf_save", "保存光场");
    actions->addWidget(start_, 0, 0, 1, 2);
    actions->addWidget(pause_, 1, 0);
    actions->addWidget(cancel_, 1, 1);
    actions->addWidget(open_, 2, 0);
    actions->addWidget(save_, 2, 1);
    layout->addLayout(actions);
    info_ = new QLabel(this);
    info_->setObjectName("lf_info");
    info_->setWordWrap(true);
    layout->addWidget(info_);
    focus_fields_ = new QWidget(this);
    auto* optical = form(focus_fields_);
    mode_ = new QComboBox(this);
    mode_->setObjectName("lf_mode");
    mode_->addItems({"重聚焦", "子视图"});
    focus_ = scalar(this, "lf_focus", 1e-6, 1e6, 4);
    aperture_ = scalar(this, "lf_aperture", 0, 1e6, .04);
    focus_slider_ = slider(this, "lf_focus_slider");
    aperture_slider_ = slider(this, "lf_aperture_slider");
    column_ = integer(this, "lf_column", 1, 15, 1);
    row_ = integer(this, "lf_row", 1, 15, 1);
    optical->addRow("图像", mode_);
    optical->addRow("焦距 / m", focus_);
    optical->addRow(focus_slider_);
    optical->addRow("孔径半径 / m", aperture_);
    optical->addRow(aperture_slider_);
    optical->addRow("子视图列", column_);
    optical->addRow("子视图行", row_);
    focus_shortcuts_ = new QWidget(focus_fields_);
    auto* shortcuts = new QHBoxLayout(focus_shortcuts_);
    shortcuts->setContentsMargins(0, 0, 0, 0);
    for (const auto& item :
         {std::tuple{"lf_near_focus", "近焦", .5}, std::tuple{"lf_reference_focus", "参考焦距", 1.},
          std::tuple{"lf_far_focus", "远焦", 1.5}}) {
        auto* b = button(focus_shortcuts_, std::get<0>(item), std::get<1>(item));
        shortcuts->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, factor = std::get<2>(item)] {
            if (controller_.file()) {
                focus_->setValue(controller_.file()->calibration().reference_focus * factor);
            }
        });
    }
    optical->addRow(focus_shortcuts_);
    layout->addWidget(focus_fields_);
    auto* display = new QFormLayout;
    exposure_ = scalar(this, "lf_exposure", -10, 10, 0);
    exposure_->setDecimals(2);
    exposure_->setSingleStep(.25);
    display->addRow("显示曝光 / EV", exposure_);
    layout->addLayout(display);
    show_ = new QCheckBox("查看光场图像", this);
    show_->setObjectName("lf_show");
    layout->addWidget(show_);
    auto* exports = new QHBoxLayout;
    exr_ = button(this, "lf_exr", "导出 EXR");
    png_ = button(this, "lf_png", "导出 PNG");
    exports->addWidget(exr_);
    exports->addWidget(png_);
    layout->addLayout(exports);
    status_ = new QLabel(this);
    status_->setObjectName("lf_status");
    status_->setWordWrap(true);
    layout->addWidget(status_);
    progress_ = new QProgressBar(this);
    progress_->setObjectName("lf_progress");
    layout->addWidget(progress_);
    notice_ = new QLabel(this);
    notice_->setObjectName("lf_notice");
    notice_->setWordWrap(true);
    layout->addWidget(notice_);
    layout->addStretch();
    connect(demo_, &QPushButton::clicked, this, [this] {
        invoke([&] {
            if (on_demo) {
                on_demo();
            }
        });
    });
    connect(start_, &QPushButton::clicked, this, [this] {
        invoke([&] {
            const auto settings = capture_settings();
            controller_.start(snapshot, settings, integrator());
        });
    });
    connect(pause_, &QPushButton::clicked, this, [this] {
        controller_.pause_resume();
        sync();
    });
    connect(cancel_, &QPushButton::clicked, this, [this] {
        controller_.cancel();
        sync();
    });
    connect(open_, &QPushButton::clicked, this, [this] {
        auto path = QFileDialog::getOpenFileName(this, "打开光场", {}, "MiniRender (*.mrlf)");
        if (!path.isEmpty()) {
            invoke([&] {
                auto budget = std::uint64_t(memory_->value()) << 20;
                controller_.open(path, budget, std::min<std::uint64_t>(64ULL << 20, budget / 4));
            });
        }
    });
    connect(save_, &QPushButton::clicked, this, [this] {
        auto path = QFileDialog::getSaveFileName(this, "保存光场", {}, "MiniRender (*.mrlf)");
        if (!path.isEmpty()) {
            invoke([&] { controller_.save(path); });
        }
    });
    for (auto* target : {exr_, png_}) {
        connect(target, &QPushButton::clicked, this, [this, target] {
            const bool exr = target == exr_;
            auto path = QFileDialog::getSaveFileName(this, "导出光场图像", {},
                                                     exr ? "OpenEXR (*.exr)" : "PNG (*.png)");
            if (!path.isEmpty()) {
                invoke([&] { controller_.export_image(path, exr); });
            }
        });
    }
    for (auto* field : {focus_, aperture_}) {
        connect(field, &QDoubleSpinBox::valueChanged, this, [this] { request(); });
    }
    for (auto* field : {column_, row_}) {
        connect(field, &QSpinBox::valueChanged, this, [this] {
            if (mode_->currentIndex() == 1) {
                request();
            }
        });
    }
    connect(mode_, &QComboBox::currentIndexChanged, this, [this] {
        request();
        sync();
    });
    connect(focus_slider_, &QSlider::valueChanged, this, [this](int v) {
        if (!syncing_ && controller_.file()) {
            focus_->setValue(controller_.file()->calibration().reference_focus *
                             std::exp2((v - 500) / 250.));
        }
    });
    connect(aperture_slider_, &QSlider::valueChanged, this, [this](int v) {
        if (!syncing_ && controller_.file()) {
            aperture_->setValue(controller_.file()->calibration().pupil_radius * v / 1000.);
        }
    });
    connect(exposure_, &QDoubleSpinBox::valueChanged, this,
            [this](double v) { viewport_.set_exposure(float(v)); });
    connect(show_, &QCheckBox::toggled, this, [this](bool v) {
        if (!syncing_) {
            controller_.show(v);
        }
    });
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] { sync(); });
    timer->start(100);
    sync();
}
void LightFieldPanel::invoke(const std::function<void()>& work) {
    error_.clear();
    try {
        work();
    } catch (const std::exception& e) {
        error_ = QString::fromUtf8(e.what());
    }
    sync();
}
LightFieldCaptureSettings LightFieldPanel::capture_settings() const {
    LightFieldCaptureSettings s;
    bool ok = false;
    auto seed = seed_->text().toULongLong(&ok);
    if (!ok) {
        throw std::invalid_argument("随机种子须为无符号整数。");
    }
    s.render = {std::size_t(width_->value()), std::size_t(height_->value()),
                std::uint32_t(spp_->value()), seed};
    s.columns = std::uint32_t(columns_->value());
    s.rows = std::uint32_t(rows_->value());
    s.threads = std::uint32_t(threads_->value());
    s.reference_focus = reference_->value();
    s.aperture_radius = radius_->value();
    s.memory_budget_bytes = std::uint64_t(memory_->value()) << 20;
    return s;
}
void LightFieldPanel::set_capture_settings(LightFieldCaptureSettings s) {
    width_->setValue(int(s.render.width));
    height_->setValue(int(s.render.height));
    spp_->setValue(int(s.render.samples_per_pixel));
    seed_->setText(QString::number(s.render.seed));
    columns_->setValue(int(s.columns));
    rows_->setValue(int(s.rows));
    threads_->setValue(int(s.threads));
    reference_->setValue(s.reference_focus);
    radius_->setValue(s.aperture_radius);
    memory_->setValue(int(s.memory_budget_bytes >> 20));
}
void LightFieldPanel::set_asset_busy(bool busy) {
    asset_busy_ = busy;
    sync();
}
void LightFieldPanel::request() {
    if (syncing_ || !controller_.file()) {
        return;
    }
    invoke([&] {
        std::optional<std::size_t> view;
        if (mode_->currentIndex() == 1) {
            view = std::size_t(row_->value() - 1) * controller_.file()->calibration().columns +
                   std::size_t(column_->value() - 1);
        }
        controller_.reconstruct(
            focus_->value(),
            std::min(aperture_->value(), controller_.file()->calibration().pupil_radius), view);
    });
}
void LightFieldPanel::sync() {
    if (syncing_) {
        return;
    }
    syncing_ = true;
    const auto data = controller_.file();
    const auto p = controller_.capture().progress();
    const auto job = controller_.job();
    using Job = LightFieldController::Job;
    const bool idle = job == Job::idle && !controller_.acquiring();
    if (data && revision_ != controller_.data_revision()) {
        revision_ = controller_.data_revision();
        const auto& c = data->calibration();
        mode_->setCurrentIndex(0);
        focus_->setValue(c.reference_focus);
        aperture_->setMaximum(c.pupil_radius);
        aperture_->setValue(c.pupil_radius);
        column_->setMaximum(int(c.columns));
        row_->setMaximum(int(c.rows));
        column_->setValue(int(c.columns / 2) + 1);
        row_->setValue(int(c.rows / 2) + 1);
        info_->setText(QString("%1×%2 px · %3×%4 视图 · %5 条采集路径\n参考焦距 %6 m · 采集半径 %7 "
                               "m · 垂直视场 %8°")
                           .arg(c.render.width)
                           .arg(c.render.height)
                           .arg(c.columns)
                           .arg(c.rows)
                           .arg(data->traced_paths())
                           .arg(c.reference_focus)
                           .arg(c.pupil_radius)
                           .arg(c.camera.vertical_fov_degrees));
    } else if (!data) {
        info_->setText("尚无完整光场");
    }
    if (data) {
        const auto& c = data->calibration();
        focus_slider_->setValue(int(
            std::clamp(500. + 250. * std::log2(focus_->value() / c.reference_focus), 0., 1000.)));
        aperture_slider_->setValue(
            int(std::clamp(aperture_->value() / c.pupil_radius * 1000., 0., 1000.)));
    }
    capture_fields_->setEnabled(idle && !asset_busy_);
    start_->setEnabled(idle && !asset_busy_);
    demo_->setEnabled(idle && !asset_busy_);
    open_->setEnabled(idle);
    save_->setEnabled(idle && bool(data));
    exr_->setEnabled(idle && bool(controller_.image()));
    png_->setEnabled(exr_->isEnabled());
    pause_->setEnabled(p.state == RenderState::running || p.state == RenderState::pausing ||
                       p.state == RenderState::paused);
    pause_->setText(p.state == RenderState::paused || p.state == RenderState::pausing ? "恢复"
                                                                                      : "暂停");
    cancel_->setEnabled(controller_.acquiring() || job != Job::idle);
    focus_fields_->setEnabled(data && !controller_.acquiring() &&
                              (idle || job == Job::focus || job == Job::view));
    const bool ref = mode_->currentIndex() == 0;
    focus_shortcuts_->setEnabled(ref);
    for (QWidget* w :
         std::initializer_list<QWidget*>{focus_, aperture_, focus_slider_, aperture_slider_}) {
        w->setEnabled(ref);
    }
    column_->setEnabled(!ref);
    row_->setEnabled(!ref);
    show_->setEnabled(bool(controller_.image()) || controller_.acquiring());
    show_->setChecked(viewport_.offline_visible() &&
                      viewport_.offline_source() == OfflineImageSource::lightfield);
    {
        const QSignalBlocker blocker(exposure_);
        exposure_->setValue(viewport_.exposure());
    }
    std::uint64_t done = 0, total = 0;
    if (controller_.acquiring() && job != Job::adopt) {
        done = p.recorded_samples;
        total = p.total_samples;
        status_->setText(
            QString("%1 · %2\n已写入 %3 / %4 视图 · %5 / %6 样本\n本阶段 %7 / %8 · 工作约 %9 MiB · "
                    "像素 %10 MiB")
                .arg(QString::fromStdString(render_state_name(p.state)), phase(p.phase))
                .arg(p.completed_views)
                .arg(p.total_views)
                .arg(done)
                .arg(total)
                .arg(p.work_completed)
                .arg(p.work_total)
                .arg(double(p.estimated_bytes) / 1048576., 0, 'f', 1)
                .arg(double(p.payload_bytes) / 1048576., 0, 'f', 1));
    } else if (job != Job::idle) {
        auto w = controller_.work();
        done = w.first;
        total = w.second;
        const QString label = job == Job::save           ? "保存光场"
                              : job == Job::image_export ? "导出图像"
                              : job == Job::load         ? "读取并重建"
                              : job == Job::view         ? "读取子视图"
                                                         : "重聚焦";
        status_->setText(QString("%1 · %2 / %3").arg(label).arg(done).arg(total));
    } else {
        status_->setText(
            controller_.message() +
            (data ? QString(" · %1 个视图参与 · 重建射线 0").arg(controller_.selected_views())
                  : ""));
        done = (data || p.job_id) ? 1 : 0;
        total = 1;
    }
    progress_->setRange(0, total ? 1000 : 0);
    if (total) {
        progress_->setValue(int(1000. * double(done) / double(total)));
    }
    notice_->setText(error_ + (controller_.stale() ? " 场景已修改，采集数据对应启动快照。" : ""));
    syncing_ = false;
}
