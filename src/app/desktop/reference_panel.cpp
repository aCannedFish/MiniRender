// 后台参考渲染复用 RenderSession 的采样进度和取消。只发布仍匹配当前视口的结果。
#include "reference_panel.hpp"
#include "vulkan_viewport.hpp"
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTimer>

namespace {
bool same_camera(const minirender::Camera& a, const minirender::Camera& b) {
    return a.position == b.position && a.target == b.target && a.up == b.up &&
           a.vertical_fov_degrees == b.vertical_fov_degrees &&
           a.aperture_radius == b.aperture_radius && a.focus_distance == b.focus_distance &&
           a.near_plane == b.near_plane && a.far_plane == b.far_plane;
}
} // namespace

ReferencePanel::ReferencePanel(VulkanViewport& viewport, QComboBox& right, QWidget* parent)
    : QWidget(parent), viewport_(viewport), right_(right) {
    auto* form = new QFormLayout(this);
    form->setContentsMargins(0, 0, 0, 0);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    size_ = new QSpinBox;
    size_->setRange(32, 2048);
    size_->setValue(256);
    size_->setSingleStep(32);
    size_->setObjectName("reference_size");
    samples_ = new QSpinBox;
    samples_->setRange(1, 4096);
    samples_->setValue(64);
    samples_->setObjectName("reference_spp");
    auto* row = new QHBoxLayout;
    row->addWidget(size_);
    row->addWidget(samples_);
    size_->setSuffix(" px");
    samples_->setSuffix(" spp");
    size_->setToolTip("参考图的最长边，按视口宽高比确定另一边");
    samples_->setToolTip("每像素路径样本数");
    form->addRow("离线参考", row);
    auto* actions = new QHBoxLayout;
    run_          = new QPushButton("生成路径追踪参考");
    cancel_       = new QPushButton("取消");
    run_->setObjectName("reference_render");
    cancel_->setEnabled(false);
    actions->addWidget(run_);
    actions->addWidget(cancel_);
    form->addRow(actions);
    progress_ = new QProgressBar;
    progress_->setRange(0, 1000);
    progress_->hide();
    form->addRow(progress_);
    status_ = new QLabel;
    status_->setObjectName("reference_status");
    status_->setWordWrap(true);
    status_->hide();
    form->addRow(status_);
    auto* model = qobject_cast<QStandardItemModel*>(right_.model());
    model->item(VulkanViewport::offline_reference_mode + 1)->setEnabled(false);
    connect(run_, &QPushButton::clicked, this, [this] { start(); });
    connect(cancel_, &QPushButton::clicked, this, [this] { session_.cancel(); });
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] { poll(); });
    timer->start(100);
}

ReferencePanel::~ReferencePanel() {
    session_.cancel();
}

void ReferencePanel::start() {
    if (!viewport_.scene() || !viewport_.isValid() || running_) {
        return;
    }
    const auto extent    = viewport_.swapChainImageSize();
    aspect_              = float(extent.width()) / float(extent.height());
    const auto long_edge = size_->value();
    const auto width     = std::size_t(
        aspect_ >= 1 ? long_edge : std::max(1L, std::lround(float(long_edge) * aspect_)));
    const auto height = std::size_t(
        aspect_ <= 1 ? long_edge : std::max(1L, std::lround(float(long_edge) / aspect_)));
    source_          = viewport_.scene();
    camera_          = viewport_.camera();
    auto snapshot    = std::make_shared<minirender::SceneSnapshot>(*source_);
    snapshot->camera = camera_;
    try {
        session_.start(snapshot,
                       {width, height, static_cast<std::uint32_t>(samples_->value()), 101},
                       {.maximum_depth = 10}, {.threads = 4, .tile_size = 8, .batch_samples = 4});
        running_ = true;
        run_->setEnabled(false);
        cancel_->setEnabled(true);
        status_->show();
        status_->setText("正在按当前相机渲染参考图…");
        progress_->setValue(0);
        progress_->show();
    } catch (const std::exception& error) {
        status_->show();
        status_->setText(QString::fromUtf8(error.what()));
    }
}

void ReferencePanel::poll() {
    auto* model = qobject_cast<QStandardItemModel*>(right_.model());
    auto* item  = model->item(VulkanViewport::offline_reference_mode + 1);
    if (published_ && !viewport_.reference_image()) {
        published_ = false;
        item->setEnabled(false);
        if (right_.currentIndex() == VulkanViewport::offline_reference_mode + 1) {
            right_.setCurrentIndex(0);
        }
        status_->setText("场景、相机或视口比例已改变，需重新生成参考");
        status_->show();
    }
    if (!running_) {
        return;
    }
    const auto p       = session_.progress();
    const auto extent  = viewport_.swapChainImageSize();
    const bool changed = source_ != viewport_.scene() ||
                         !same_camera(camera_, viewport_.camera()) || extent.height() <= 0 ||
                         std::abs(aspect_ - float(extent.width()) / float(extent.height())) > .001F;
    if (!minirender::render_terminal(p.state)) {
        if (changed) {
            session_.cancel();
        }
        progress_->setValue(
            p.total_samples ? static_cast<int>(1000 * p.recorded_samples / p.total_samples) : 0);
        return;
    }
    running_ = false;
    run_->setEnabled(true);
    cancel_->setEnabled(false);
    progress_->hide();
    if (p.state != minirender::RenderState::completed || changed) {
        status_->setText(changed           ? "输入已变化，参考任务已结束"
                         : p.error.empty() ? "参考任务已取消"
                                           : QString::fromStdString(p.error));
        return;
    }
    auto image = session_.preview(0).image;
    if (!image) {
        status_->setText("任务结束时缺少参考图像");
        return;
    }
    viewport_.set_reference_image(image);
    published_ = true;
    item->setEnabled(true);
    right_.setCurrentIndex(VulkanViewport::offline_reference_mode + 1);
    status_->setText(
        QString("右侧为路径追踪参考 · %1 × %2").arg(image->width()).arg(image->height()));
}
