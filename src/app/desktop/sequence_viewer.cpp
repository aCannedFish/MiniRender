// 读取和校验选中的线性 EXR，在工作线程完成显示转换，GUI 线程负责播放控制。
#include "sequence_viewer.hpp"
#include "workbench_internal.hpp"
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <minirender/core/image.hpp>

namespace {
QImage read_frame(const QString& root, const catalog::File& file) {
    catalog::verify(root, file);
    const auto frame = minirender::Image::read_exr(
        workbench_detail::native_path(catalog::resolve(root, file.path)));
    QImage image(static_cast<int>(frame.width()), static_cast<int>(frame.height()),
                 QImage::Format_RGB888);
    if (image.isNull()) {
        throw std::runtime_error("无法分配帧缓冲区");
    }
    for (int y = 0; y < image.height(); ++y) {
        auto* out = image.scanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            const auto& pixel = frame.at(static_cast<std::size_t>(x), static_cast<std::size_t>(y));
            for (int channel = 0; channel < 3; ++channel) {
                *out++ = minirender::encode_unorm8(minirender::linear_to_srgb(pixel[channel]));
            }
        }
    }
    return image;
}
} // namespace

SequenceViewer::SequenceViewer(catalog::Entry entry, QWidget* parent)
    : QDialog(parent), entry_(std::move(entry)) {
    setWindowTitle(entry_.name);
    setObjectName("sequence_viewer");
    setAttribute(Qt::WA_DeleteOnClose);
    resize(640, 480);
    auto* layout = new QVBoxLayout(this);
    image_       = new QLabel;
    image_->setAlignment(Qt::AlignCenter);
    image_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    layout->addWidget(image_, 1);
    status_ = new QLabel("正在读取帧清单…");
    status_->setObjectName("frame_status");
    layout->addWidget(status_);
    auto* bar = new QHBoxLayout;
    layout->addLayout(bar);
    auto* play = new QPushButton("播放");
    play->setCheckable(true);
    play->setEnabled(false);
    slider_ = new QSlider(Qt::Horizontal);
    slider_->setEnabled(false);
    bar->addWidget(play);
    bar->addWidget(slider_, 1);
    timer_ = new QTimer(this);
    timer_->setInterval(100);
    connect(play, &QPushButton::toggled, this, [this, play](bool active) {
        play->setText(active ? "暂停" : "播放");
        if (active) {
            timer_->start();
        } else {
            timer_->stop();
        }
    });
    connect(timer_, &QTimer::timeout, this, [this] {
        if (!loading_) {
            slider_->setValue((slider_->value() + 1) % frames_.size());
        }
    });
    connect(slider_, &QSlider::valueChanged, this, [this] { request_frame(); });
    workbench_detail::run_background(
        this, [entry = entry_] { return catalog::frames(entry); },
        [this, play](const auto& result) {
            if (!result.error.empty()) {
                status_->setText(QString::fromStdString(result.error));
                return;
            }
            frames_ = result.value;
            slider_->setRange(0, static_cast<int>(frames_.size() - 1));
            slider_->setEnabled(true);
            play->setEnabled(frames_.size() > 1);
            request_frame();
        });
}

void SequenceViewer::request_frame() {
    if (loading_ || frames_.empty()) {
        return;
    }
    const int index = slider_->value();
    loading_        = true;
    status_->setText(QString("正在读取第 %1 帧…").arg(index + 1));
    workbench_detail::run_background(
        this, [root = entry_.root, file = frames_[index]] { return read_frame(root, file); },
        [this, index](const auto& result) {
            loading_ = false;
            if (index != slider_->value()) {
                request_frame();
                return;
            }
            if (!result.error.empty()) {
                findChild<QPushButton*>()->setChecked(false);
                status_->setText(QString::fromStdString(result.error));
                return;
            }
            current_ = QPixmap::fromImage(result.value);
            update_image();
            status_->setText(QString("%1 / %2   ·   %3 × %4")
                                 .arg(index + 1)
                                 .arg(frames_.size())
                                 .arg(current_.width())
                                 .arg(current_.height()));
        });
}

void SequenceViewer::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    update_image();
}

void SequenceViewer::update_image() {
    if (!current_.isNull()) {
        image_->setPixmap(
            current_.scaled(image_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
}
