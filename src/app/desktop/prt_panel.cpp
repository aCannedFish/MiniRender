// 烘焙与环境投影均在工作线程执行。改变环境图或旋转时复用静态传输数据。
#include "prt_panel.hpp"
#include "prt_cache.hpp"
#include "workbench_internal.hpp"
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTimer>

PrtPanel::PrtPanel(VulkanViewport& viewport, QWidget* parent)
    : QWidget(parent), viewport_(viewport) {
    auto* form = new QFormLayout(this);
    workbench_detail::configure_form(form);
    mode_ = new QComboBox;
    mode_->setObjectName("prt_method");
    mode_->addItems({"无遮挡", "静态遮挡", "漫反射互反射"});
    mode_->setCurrentIndex(1);
    samples_ = new QSpinBox;
    samples_->setRange(8, 4096);
    samples_->setValue(128);
    samples_->setObjectName("prt_samples");
    bounces_ = new QSpinBox;
    bounces_->setRange(1, 8);
    bounces_->setValue(1);
    bounces_->setEnabled(false);
    form->addRow("PRT 传输", mode_);
    form->addRow("每顶点样本", samples_);
    form->addRow("反弹次数", bounces_);
    auto* row = new QHBoxLayout;
    bake_     = new QPushButton("烘焙并预览");
    bake_->setObjectName("prt_bake");
    cancel_ = new QPushButton("取消");
    cancel_->setEnabled(false);
    row->addWidget(bake_);
    row->addWidget(cancel_);
    form->addRow(row);
    progress_ = new QProgressBar;
    progress_->hide();
    form->addRow(progress_);
    status_ = new QLabel("静态几何的传输按场景缓存");
    status_->setObjectName("prt_status");
    status_->setWordWrap(true);
    form->addRow(status_);
    connect(mode_, &QComboBox::currentIndexChanged, this, [this](int index) {
        samples_->setEnabled(index > 0);
        bounces_->setEnabled(index == 2);
    });
    connect(bake_, &QPushButton::clicked, this, [this] { start(); });
    connect(cancel_, &QPushButton::clicked, this, [this] { cancellation_.request(); });
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] {
        if (running_) {
            progress_->setValue(percent_->load());
            return;
        }
        refresh_lighting();
    });
    timer->start(100);
}

PrtPanel::~PrtPanel() {
    cancellation_.request();
}

void PrtPanel::start() {
    const auto scene = viewport_.scene();
    if (!scene || running_) {
        return;
    }
    const minirender::PrtSettings settings{static_cast<minirender::PrtMode>(mode_->currentIndex()),
                                           static_cast<std::uint32_t>(samples_->value()),
                                           static_cast<std::uint32_t>(bounces_->value())};
    const auto path = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/prt";
    cancellation_   = minirender::CancellationSource();
    auto token      = cancellation_.token();
    percent_        = std::make_shared<std::atomic<int>>(0);
    running_        = true;
    bake_->setEnabled(false);
    cancel_->setEnabled(true);
    progress_->setRange(0, 100);
    progress_->setValue(0);
    progress_->show();
    status_->setText("正在读取缓存或烘焙传输…");
    workbench_detail::run_background(
        this,
        [scene, settings, token, path, percent = percent_] {
            return load_or_bake_prt(path, scene, settings, token, [percent](auto done, auto total) {
                if (total) {
                    *percent = static_cast<int>(100 * done / total);
                }
            });
        },
        [this, scene, settings, token](const auto& result) {
            running_ = false;
            progress_->hide();
            bake_->setEnabled(true);
            cancel_->setEnabled(false);
            if (token.requested()) {
                status_->setText("已取消烘焙");
                return;
            }
            if (!result.error.empty()) {
                status_->setText(QString::fromStdString(result.error));
                return;
            }
            if (!viewport_.scene() ||
                !minirender::compatible_prt(*scene, *viewport_.scene(), settings.mode)) {
                status_->setText("烘焙期间几何发生变化，请重新烘焙");
                return;
            }
            viewport_.set_prt(result.value.data, settings.mode);
            last_scene_.reset();
            refresh_lighting();
            window()->findChild<QComboBox*>("view_mode")->setCurrentIndex(VulkanViewport::prt_mode);
            status_->setText(result.value.reused ? "已载入静态传输缓存" : "已完成烘焙并保存缓存");
        });
}

void PrtPanel::refresh_lighting() {
    auto scene = viewport_.scene();
    if (!viewport_.prt()) {
        if (last_scene_) {
            status_->setText("几何或材质已变更，需重新烘焙");
        }
        last_scene_.reset();
        return;
    }
    if (!scene || last_scene_ == scene || projecting_) {
        return;
    }
    if (source_ != scene->environment_texture) {
        if (!scene->environment_texture) {
            source_.reset();
            projected_ = {};
        } else {
            projecting_       = true;
            const auto source = scene->environment_texture;
            workbench_detail::run_background(
                this, [source] { return minirender::project_environment(source->level(0)); },
                [this, source](const auto& result) {
                    projecting_ = false;
                    if (!result.error.empty()) {
                        status_->setText(QString::fromStdString(result.error));
                        return;
                    }
                    source_    = source;
                    projected_ = result.value;
                    refresh_lighting();
                });
            return;
        }
    }
    auto lighting = minirender::constant_environment(scene->environment);
    if (source_) {
        const auto rotated = minirender::rotate_environment(
            projected_, glm::radians(double(scene->environment_rotation_degrees)));
        for (std::size_t k = 0; k < 9; ++k) {
            lighting[k] += rotated[k] * double(scene->environment_strength);
        }
    }
    viewport_.set_prt_lighting(lighting);
    last_scene_ = scene;
}
