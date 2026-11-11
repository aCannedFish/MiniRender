#include "offline_panel.hpp"
#include "vulkan_viewport.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <thread>

namespace {

// 显示最短可往返的数字，保留 QDoubleSpinBox 的输入、步进和范围校验。
class SppmScalar final : public QDoubleSpinBox {
  public:
    using QDoubleSpinBox::QDoubleSpinBox;

  protected:
    QString textFromValue(double value) const override {
        return locale().toString(value, 'g', QLocale::FloatingPointShortest);
    }
};

QDoubleSpinBox* scalar(QWidget* parent, const char* name, double low, double high, double step) {
    auto* spin = new SppmScalar(parent);
    spin->setObjectName(name);
    spin->setDecimals(323);
    spin->setRange(low, high);
    spin->setSingleStep(step);
    spin->setKeyboardTracking(false);
    return spin;
}

QString photon_phase(minirender::SppmPhase phase) {
    using minirender::SppmPhase;
    switch (phase) {
    case SppmPhase::camera:
        return "相机路径";
    case SppmPhase::grid:
        return "可见点网格";
    case SppmPhase::photons:
        return "光子追踪";
    case SppmPhase::update:
        return "统计更新";
    }
    return {};
}

QSpinBox* integer(QWidget* parent, const char* name, int low, int high, int value) {
    auto* spin = new QSpinBox(parent);
    spin->setObjectName(name);
    spin->setRange(low, high);
    spin->setValue(value);
    spin->setKeyboardTracking(false);
    return spin;
}

QString stage(minirender::RenderState state) {
    using minirender::RenderState;
    switch (state) {
    case RenderState::idle:
        return "尚未开始";
    case RenderState::preparing:
        return "正在准备场景";
    case RenderState::running:
        return "正在采样";
    case RenderState::pausing:
        return "正在暂停";
    case RenderState::paused:
        return "已暂停";
    case RenderState::finalizing:
        return "正在整理结果";
    case RenderState::cancelling:
        return "正在取消";
    case RenderState::cancelled:
        return "已取消，保留部分结果";
    case RenderState::completed:
        return "渲染完成";
    case RenderState::failed:
        return "渲染失败";
    }
    return {};
}

} // namespace

OfflinePanel::OfflinePanel(
    VulkanViewport& viewport,
    std::function<std::shared_ptr<const minirender::SceneSnapshot>()> snapshot, QWidget* parent)
    : QWidget(parent), viewport_(viewport), snapshot_(std::move(snapshot)),
      session_(std::make_shared<minirender::RenderSession>()) {
    setObjectName("offline_panel");
    auto* layout = new QVBoxLayout(this);
    settings_ = new QWidget(this);
    auto* form = new QFormLayout(settings_);
    form->setContentsMargins(0, 0, 0, 0);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    backend_ = new QComboBox(settings_);
    backend_->setObjectName("offline_backend");
    backend_->addItems({"Path Tracing", "SPPM"});
    width_ = integer(settings_, "offline_width", 1, 16384, 256);
    height_ = integer(settings_, "offline_height", 1, 16384, 256);
    spp_ = integer(settings_, "offline_spp", 1, 1000000, 64);
    depth_ = integer(settings_, "offline_depth", 0, 128, 8);
    threads_ = integer(settings_, "offline_threads", 0, 32, 0);
    threads_->setSpecialValueText("自动，保留一个 CPU 核");
    seed_ = new QLineEdit("1", settings_);
    seed_->setObjectName("offline_seed");
    seed_->setValidator(new QRegularExpressionValidator(QRegularExpression("[0-9]{1,20}"), seed_));
    mode_ = new QComboBox(settings_);
    mode_->setObjectName("offline_mode");
    mode_->addItems({"方向采样", "光源采样", "MIS"});
    mode_->setToolTip("方向采样在表面使用 BSDF，在介质内使用相函数。");
    mode_->setCurrentIndex(2);
    rr_ = new QCheckBox("启用 Russian roulette", settings_);
    rr_->setObjectName("offline_rr");
    rr_->setChecked(true);
    rr_depth_ = integer(settings_, "offline_rr_depth", 0, 128, 3);
    form->addRow("积分器", backend_);
    form->addRow("宽 / px", width_);
    form->addRow("高 / px", height_);
    samples_label_ = new QLabel("每像素样本数", settings_);
    form->addRow(samples_label_, spp_);
    form->addRow("最大散射深度", depth_);
    form->addRow("采样方式", mode_);
    form->addRow("随机种子", seed_);
    form->addRow("工作线程", threads_);
    form->addRow(rr_);
    form->addRow("RR 起始深度", rr_depth_);
    sppm_settings_ = new QWidget(settings_);
    auto* photon_form = new QFormLayout(sppm_settings_);
    photon_form->setContentsMargins(0, 0, 0, 0);
    photon_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    photon_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    photons_ = integer(sppm_settings_, "offline_photons", 1, 100000000, 100000);
    radius_ = scalar(sppm_settings_, "offline_radius", 1e-6, 1e6, .01);
    radius_->setValue(sppm_options_.initial_radius);
    alpha_ = scalar(sppm_settings_, "offline_alpha", 0, 1, .05);
    alpha_->setValue(sppm_options_.alpha);
    memory_ = integer(sppm_settings_, "offline_sppm_memory", 1, 8192, 512);
    radius_->setToolTip("初始合并半径，单位为场景中的米。后续迭代逐渐缩小半径。");
    alpha_->setToolTip("半径更新系数，取值在 0 和 1 之间，通常使用 0.7。");
    memory_->setToolTip("SPPM 工作缓冲预算，场景资产和 BVH 另计。");
    photon_form->addRow("每轮发射光子数", photons_);
    photon_form->addRow("初始半径 / m", radius_);
    photon_form->addRow("更新系数 α", alpha_);
    photon_form->addRow("内存预算 / MiB", memory_);
    auto* hint = new QLabel("SPPM 使用针孔相机计算表面散射。每轮为每个像素追踪一条相机路径。",
                            sppm_settings_);
    hint->setWordWrap(true);
    photon_form->addRow(hint);
    form->addRow(sppm_settings_);
    layout->addWidget(settings_);
    auto* buttons = new QGridLayout;
    start_ = new QPushButton("开始", this);
    start_->setObjectName("offline_start");
    pause_ = new QPushButton("暂停", this);
    pause_->setObjectName("offline_pause");
    cancel_ = new QPushButton("取消", this);
    cancel_->setObjectName("offline_cancel");
    save_exr_ = new QPushButton("导出 EXR", this);
    save_exr_->setObjectName("offline_save_exr");
    save_png_ = new QPushButton("导出 PNG", this);
    save_png_->setObjectName("offline_save_png");
    buttons->addWidget(start_, 0, 0, 1, 2);
    buttons->addWidget(pause_, 1, 0);
    buttons->addWidget(cancel_, 1, 1);
    buttons->addWidget(save_exr_, 2, 0);
    buttons->addWidget(save_png_, 2, 1);
    layout->addLayout(buttons);
    show_ = new QCheckBox("查看离线结果（取消勾选返回实时预览）", this);
    show_->setObjectName("offline_show");
    layout->addWidget(show_);
    auto* display = new QFormLayout;
    exposure_ = new QDoubleSpinBox(this);
    exposure_->setObjectName("offline_exposure");
    exposure_->setRange(-10, 10);
    exposure_->setSingleStep(0.25);
    exposure_->setKeyboardTracking(false);
    display->addRow("显示曝光 / EV", exposure_);
    layout->addLayout(display);
    status_ = new QLabel(this);
    status_->setObjectName("offline_status");
    details_ = new QLabel(this);
    details_->setObjectName("offline_details");
    notice_ = new QLabel(this);
    notice_->setObjectName("offline_notice");
    for (auto* label : {status_, details_, notice_}) {
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(label);
    }
    progress_ = new QProgressBar(this);
    progress_->setObjectName("offline_progress");
    layout->addWidget(progress_);
    sppm_details_ = new QLabel(this);
    sppm_details_->setObjectName("offline_sppm_details");
    sppm_details_->setWordWrap(true);
    sppm_details_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(sppm_details_);
    // macOS 原生进度条省略条内文字，阶段与计数放在独立标签中。
    phase_status_ = new QLabel(this);
    phase_status_->setObjectName("offline_phase_status");
    phase_status_->setWordWrap(true);
    layout->addWidget(phase_status_);
    phase_progress_ = new QProgressBar(this);
    phase_progress_->setObjectName("offline_phase_progress");
    layout->addWidget(phase_progress_);
    layout->addStretch();
    connect(start_, &QPushButton::clicked, this, [this] { start(); });
    connect(pause_, &QPushButton::clicked, this, [this] {
        const auto state = session_->progress().state;
        if (state == minirender::RenderState::paused || state == minirender::RenderState::pausing) {
            session_->resume();
        } else {
            session_->pause();
        }
        poll();
    });
    connect(cancel_, &QPushButton::clicked, this, [this] {
        session_->cancel();
        poll();
    });
    connect(save_exr_, &QPushButton::clicked, this, [this] { choose_export(true); });
    connect(save_png_, &QPushButton::clicked, this, [this] { choose_export(false); });
    connect(show_, &QCheckBox::toggled, this,
            [this](bool visible) { viewport_.set_offline_visible(visible); });
    connect(exposure_, &QDoubleSpinBox::valueChanged, this,
            [this](double value) { viewport_.set_exposure(static_cast<float>(value)); });
    const auto parameters_changed = [this] { scene_changed(); };
    for (auto* field : {width_, height_, spp_, depth_, threads_, rr_depth_}) {
        connect(field, &QSpinBox::valueChanged, this, parameters_changed);
    }
    connect(seed_, &QLineEdit::textChanged, this, parameters_changed);
    connect(mode_, &QComboBox::currentIndexChanged, this, parameters_changed);
    connect(rr_, &QCheckBox::toggled, this, parameters_changed);
    connect(backend_, &QComboBox::currentIndexChanged, this, [this] {
        update_backend_controls();
        scene_changed();
    });
    connect(photons_, &QSpinBox::valueChanged, this, [this](int value) {
        sppm_options_.photons_per_iteration = static_cast<std::uint32_t>(value);
        scene_changed();
    });
    connect(radius_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        sppm_options_.initial_radius = value;
        scene_changed();
    });
    connect(alpha_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        sppm_options_.alpha = value;
        scene_changed();
    });
    connect(memory_, &QSpinBox::valueChanged, this, [this](int value) {
        sppm_options_.memory_budget_bytes = static_cast<std::uint64_t>(value) * 1024 * 1024;
        memory_->setToolTip(
            QString("SPPM 工作缓冲预算，当前精确值为 %1 字节。场景资产和 BVH 另计。")
                .arg(sppm_options_.memory_budget_bytes));
        scene_changed();
    });
    update_backend_controls();
    auto* timer = new QTimer(this);
    timer->setInterval(200);
    connect(timer, &QTimer::timeout, this, [this] { poll(); });
    timer->start();
    poll();
}

OfflinePanel::~OfflinePanel() {
    session_->cancel();
    session_->wait();
}
void OfflinePanel::set_settings(minirender::RenderSettings render,
                                minirender::IntegratorSettings integrator,
                                minirender::SessionSettings session) {
    render.validate();
    integrator.validate();
    session.validate();
    if (render.samples_per_pixel > static_cast<std::uint32_t>(spp_->maximum())) {
        throw std::invalid_argument("Offline panel supports at most 1,000,000 samples per pixel");
    }
    if (!minirender::render_terminal(session_->progress().state) || exporting_) {
        throw std::logic_error("Cannot change active render settings");
    }
    width_->setValue(static_cast<int>(render.width));
    height_->setValue(static_cast<int>(render.height));
    spp_->setValue(static_cast<int>(render.samples_per_pixel));
    seed_->setText(QString::number(render.seed));
    depth_->setValue(static_cast<int>(integrator.maximum_depth));
    mode_->setCurrentIndex(static_cast<int>(integrator.mode));
    rr_->setChecked(integrator.russian_roulette);
    rr_depth_->setValue(static_cast<int>(integrator.roulette_depth));
    threads_->setValue(static_cast<int>(session.threads));
    session_settings_ = session;
    const QSignalBlocker photons_blocker(photons_), radius_blocker(radius_), alpha_blocker(alpha_),
        memory_blocker(memory_);
    sppm_options_ = integrator.sppm;
    photons_->setValue(static_cast<int>(sppm_options_.photons_per_iteration));
    radius_->setValue(sppm_options_.initial_radius);
    alpha_->setValue(sppm_options_.alpha);
    memory_->setValue(static_cast<int>(sppm_options_.memory_budget_bytes / (1024 * 1024)));
    memory_->setToolTip(QString("SPPM 工作缓冲预算，当前精确值为 %1 字节。场景资产和 BVH 另计。")
                            .arg(sppm_options_.memory_budget_bytes));
    backend_->setCurrentIndex(integrator.kind == minirender::IntegratorKind::sppm ? 1 : 0);
    update_backend_controls();
}

void OfflinePanel::update_backend_controls() {
    const bool sppm = backend_->currentIndex() == 1;
    sppm_settings_->setVisible(sppm);
    samples_label_->setText(sppm ? "迭代次数" : "每像素样本数");
    mode_->setEnabled(!sppm);
    mode_->setToolTip(sppm ? "SPPM 固定组合直接光采样和光子合并，无须选择 PT 采样方式。"
                           : "方向采样在表面使用 BSDF，在介质内使用相函数。");
}
void OfflinePanel::prepare_import(minirender::RenderSettings render,
                                  minirender::IntegratorSettings integrator) {
    render.validate();
    integrator.validate();
    session_->cancel();
    viewport_.set_offline_image(nullptr);
    viewport_.set_offline_visible(false);
    imported_settings_ = ImportSettings{render, integrator};
    poll();
}

void OfflinePanel::start() {
    try {
        if (exporting_) {
            throw std::logic_error("Wait for image export before starting another render");
        }
        bool valid = false;
        const auto seed = seed_->text().toULongLong(&valid);
        if (!valid) {
            throw std::invalid_argument("Seed must be an unsigned 64-bit integer");
        }
        const minirender::RenderSettings render{static_cast<std::size_t>(width_->value()),
                                                static_cast<std::size_t>(height_->value()),
                                                static_cast<std::uint32_t>(spp_->value()), seed};
        const auto integrator = integrator_settings();
        auto settings = session_settings_;
        settings.threads = static_cast<std::uint32_t>(threads_->value());
        const auto snapshot = snapshot_();
        if (integrator.kind == minirender::IntegratorKind::sppm) {
            if (snapshot->camera.aperture_radius != 0) {
                throw std::invalid_argument("SPPM 需要针孔相机，请将相机光圈半径设为 0。");
            }
            for (const auto& region : snapshot->media) {
                if (region.enabled &&
                    glm::any(glm::greaterThan(region.medium.extinction(), glm::dvec3(0)))) {
                    throw std::invalid_argument(
                        "SPPM 当前支持表面散射；含介质的场景请选择 Path Tracing。");
                }
            }
        }
        session_->start(snapshot, render, integrator, settings);
        job_iterations_ = render.samples_per_pixel;
        stale_ = false;
        preview_samples_ = 0;
        preview_terminal_ = false;
        export_message_.clear();
        viewport_.set_offline_image(nullptr);
        show_->setChecked(true);
        poll();
    } catch (const std::exception& error) {
        export_message_ = QString::fromUtf8(error.what());
        poll();
    }
}
void OfflinePanel::scene_changed() {
    if (session_->progress().job_id) {
        stale_ = true;
    }
}

void OfflinePanel::poll() {
    if (imported_settings_ && minirender::render_terminal(session_->progress().state) &&
        !exporting_) {
        const auto settings = *imported_settings_;
        imported_settings_.reset();
        set_settings(settings.render, settings.integrator, {});
    }
    using minirender::RenderState;
    const auto p = session_->progress();
    const bool terminal = minirender::render_terminal(p.state);
    settings_->setEnabled(terminal && !exporting_);
    update_backend_controls();
    rr_depth_->setEnabled(rr_->isChecked());
    start_->setEnabled(terminal && !exporting_);
    pause_->setEnabled(p.state == RenderState::running || p.state == RenderState::pausing ||
                       p.state == RenderState::paused);
    pause_->setText(p.state == RenderState::paused || p.state == RenderState::pausing ? "恢复"
                                                                                      : "暂停");
    cancel_->setEnabled(!terminal && p.state != RenderState::cancelling);
    save_exr_->setEnabled(p.recorded_samples > 0 && !exporting_);
    save_png_->setEnabled(p.recorded_samples > 0 && !exporting_);
    show_->setEnabled(p.job_id != 0);
    status_->setText(stage(p.state));
    const double percentage = p.total_samples ? 100.0 * static_cast<double>(p.recorded_samples) /
                                                    static_cast<double>(p.total_samples)
                                              : 0;
    details_->setText(QString("已记录 %1 / %2 个样本（%3%）\n%4 个线程 · %5 秒")
                          .arg(p.recorded_samples)
                          .arg(p.total_samples)
                          .arg(percentage, 0, 'f', 1)
                          .arg(p.worker_count)
                          .arg(p.elapsed_seconds, 0, 'f', 1));
    const bool sppm_job = p.job_id && p.integrator == minirender::IntegratorKind::sppm;
    sppm_details_->setVisible(sppm_job);
    phase_progress_->setVisible(sppm_job);
    phase_status_->setVisible(sppm_job);
    if (sppm_job) {
        details_->setText(QString("已完成 %1 / %2 轮（%3%）\n%4 个线程 · %5 秒")
                              .arg(p.sppm.iterations)
                              .arg(job_iterations_)
                              .arg(percentage, 0, 'f', 1)
                              .arg(p.worker_count)
                              .arg(p.elapsed_seconds, 0, 'f', 1));
        sppm_details_->setText(QString("完整迭代已发射 %1 个光子，合并 %2 次贡献\n"
                                       "平均半径 %3 m（%4–%5 m）\n工作缓冲约 %6 MiB")
                                   .arg(p.sppm.emitted_photons)
                                   .arg(p.sppm.gathered_contributions)
                                   .arg(p.sppm.mean_radius, 0, 'g', 6)
                                   .arg(p.sppm.minimum_radius, 0, 'g', 6)
                                   .arg(p.sppm.maximum_radius, 0, 'g', 6)
                                   .arg(double(p.sppm.estimated_bytes) / (1024 * 1024), 0, 'f', 1));
        phase_progress_->setRange(0, 10000);
        const bool advancing = p.state == RenderState::running;
        const auto& work = p.sppm_progress;
        phase_status_->setText(
            advancing ? QString("第 %1 轮 · %2：%3 / %4")
                            .arg(std::min(p.sppm.iterations + 1, std::uint64_t(job_iterations_)))
                            .arg(photon_phase(work.phase))
                            .arg(work.completed)
                            .arg(work.total)
                      : stage(p.state));
        phase_progress_->setValue(advancing && work.total
                                      ? int(10000.0 * double(work.completed) / double(work.total))
                                      : 0);
        phase_progress_->setFormat(advancing ? photon_phase(work.phase) + " · %p%"
                                             : stage(p.state));
    }
    QString notice;
    if (stale_) {
        notice = "场景或参数已修改，当前结果对应启动时的快照。请重新开始以更新结果。";
    }
    if (sppm_job && (p.state == RenderState::paused || p.state == RenderState::pausing)) {
        notice += "\n暂停保留完整迭代，恢复后重新计算尚未完成的一轮。";
    }
    if (!p.error.empty()) {
        notice += "\n" + QString::fromUtf8(p.error.c_str());
    }
    if (!export_message_.isEmpty()) {
        notice += "\n" + export_message_;
    }
    notice_->setText(notice.trimmed());
    const bool busy = p.state == RenderState::preparing || p.state == RenderState::finalizing ||
                      p.state == RenderState::cancelling;
    progress_->setRange(0, busy ? 0 : 10000);
    if (!busy) {
        progress_->setValue(
            p.total_samples ? static_cast<int>(10000.0 * static_cast<double>(p.recorded_samples) /
                                               static_cast<double>(p.total_samples))
                            : 0);
        progress_->setFormat("%p%");
    }
    // 实时预览期间只更新任务状态，避免为不可见的离线图反复复制和上传像素。
    if (viewport_.offline_visible() &&
        (p.recorded_samples > preview_samples_ || (terminal && !preview_terminal_ && p.job_id))) {
        const auto preview = session_->preview();
        if (preview.image) {
            viewport_.set_offline_image(preview.image);
            preview_samples_ = preview.recorded_samples;
            preview_terminal_ = terminal;
        }
    }
    const QSignalBlocker exposure_blocker(exposure_);
    exposure_->setValue(viewport_.exposure());
    const QSignalBlocker show_blocker(show_);
    show_->setChecked(viewport_.offline_visible());
}

void OfflinePanel::choose_export(bool exr) {
    const auto path = QFileDialog::getSaveFileName(this, exr ? "导出线性结果" : "导出显示图像", {},
                                                   exr ? "OpenEXR (*.exr)" : "PNG (*.png)");
    if (!path.isEmpty()) {
        save_image(path, exr);
    }
}
void OfflinePanel::save_image(const QString& path, bool exr) {
    if (exporting_ || session_->progress().recorded_samples == 0) {
        throw std::logic_error("No recorded image is available, or an export is active");
    }
    exporting_ = true;
    export_message_ = "正在导出图像…";
    poll();
    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
        const auto error = watcher->result();
        watcher->deleteLater();
        exporting_ = false;
        export_message_ = error.isEmpty() ? "图像已导出" : error;
        poll();
    });
    const auto session = session_;
    const auto exposure = viewport_.exposure();
    watcher->setFuture(QtConcurrent::run([session, path, exr, exposure] {
        try {
            auto image = session->result();
            // 完整中间图在后台复制。渲染正在提交时短暂重试，不让 UI 等待 Film 锁。
            for (int attempt = 0; !image && attempt < 1000; ++attempt) {
                image = session->preview(0).image;
                if (!image) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            if (!image) {
                throw std::runtime_error("Image is busy. Try export again");
            }
            const auto bytes = path.toUtf8();
            const auto file = std::filesystem::path(std::u8string(bytes.begin(), bytes.end()));
            if (exr) {
                image->write_exr(file);
            } else {
                minirender::display_image(*image, exposure).write_png(file);
            }
            return QString();
        } catch (const std::exception& error) {
            return QString::fromUtf8(error.what());
        }
    }));
}
bool OfflinePanel::exporting() const noexcept {
    return exporting_;
}
bool OfflinePanel::stale() const noexcept {
    return stale_;
}
minirender::IntegratorSettings OfflinePanel::integrator_settings() const {
    minirender::IntegratorSettings result{
        static_cast<minirender::SamplingMode>(mode_->currentIndex()),
        static_cast<std::uint32_t>(depth_->value()), rr_->isChecked(),
        static_cast<std::uint32_t>(rr_depth_->value())};
    result.kind = backend_->currentIndex() == 1 ? minirender::IntegratorKind::sppm
                                                : minirender::IntegratorKind::path_tracing;
    result.sppm = sppm_options_;
    return result;
}
const minirender::RenderSession& OfflinePanel::session() const noexcept {
    return *session_;
}
