// 校验 EXR 与矩阵、空间滤波、连续帧重投影和原图对照。窗口只在 GUI 线程绘图。
#include "sequence_viewer.hpp"
#include "workbench_internal.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <minirender/assets/denoise_io.hpp>
#include <minirender/render/path_tracer.hpp>

using namespace minirender;

struct ViewedFrame {
    int                                  index = -1, method = 0;
    std::shared_ptr<const Image>         original, filtered;
    std::shared_ptr<const DenoiseGuides> guides;
    QImage                               before, after;
    std::size_t                          accepted = 0;
};

// 在同一目标矩形裁剪两张全幅图。分界线拖动只触发 QWidget 重绘。
class ComparisonImage final : public QWidget {
  public:
    explicit ComparisonImage(QWidget* parent) : QWidget(parent) {
        setMinimumSize(100, 80);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void images(QImage a, QImage b) {
        a_ = std::move(a);
        b_ = std::move(b);
        update();
    }

    void compare(bool value) {
        compare_ = value;
        setCursor(value ? Qt::SplitHCursor : Qt::ArrowCursor);
        update();
    }

  private:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        if (b_.isNull()) {
            return;
        }
        const auto size = b_.size().scaled(this->size(), Qt::KeepAspectRatio);
        rect_ = QRect(QPoint((width() - size.width()) / 2, (height() - size.height()) / 2), size);
        p.drawImage(rect_, b_);
        if (compare_) {
            const int x = rect_.left() + int(double(rect_.width()) * split_);
            p.setClipRect(QRect(rect_.topLeft(), QPoint(x, rect_.bottom())));
            p.drawImage(rect_, a_);
            p.setClipping(false);
            p.setPen(Qt::white);
            p.drawLine(x, rect_.top(), x, rect_.bottom());
        }
    }

    void mouseMoveEvent(QMouseEvent* e) override {
        if (compare_ && e->buttons().testFlag(Qt::LeftButton) && rect_.width()) {
            split_ = std::clamp((e->position().x() - rect_.left()) / rect_.width(), 0., 1.);
            update();
        }
    }

    QImage a_, b_;
    QRect  rect_;
    double split_   = .5;
    bool   compare_ = false;
};

namespace {
QImage display(const Image& linear, bool tone) {
    QImage image(int(linear.width()), int(linear.height()), QImage::Format_RGB888);
    if (image.isNull()) {
        throw std::runtime_error("无法分配帧显示缓冲");
    }
    for (std::size_t y = 0; y < linear.height(); ++y) {
        auto* out = image.scanLine(int(y));
        for (std::size_t x = 0; x < linear.width(); ++x) {
            for (int c = 0; c < 3; ++c) {
                auto v = std::max(0.F, linear.at(x, y)[c]);
                if (tone) {
                    v = v / (1 + v);
                }
                *out++ = encode_unorm8(linear_to_srgb(v));
            }
        }
    }
    return image;
}

std::filesystem::path checked(const QString& root, const catalog::File& file,
                              CancellationToken token) {
    catalog::verify(root, file, [token](auto) { token.check(); });
    return workbench_detail::native_path(catalog::resolve(root, file.path));
}
} // namespace

SequenceViewer::SequenceViewer(catalog::Entry entry, QWidget* parent)
    : QDialog(parent), entry_(std::move(entry)) {
    build();
    setWindowTitle(entry_.name);
    workbench_detail::run_background(
        this, [entry = entry_] { return catalog::frames(entry); },
        [this](const auto& result) {
            if (!result.error.empty()) {
                status_->setText(QString::fromStdString(result.error));
                return;
            }
            frames_ = result.value;
            prepare_controls();
        });
}

SequenceViewer::SequenceViewer(std::function<std::shared_ptr<const Image>()> image,
                               std::shared_ptr<const SceneSnapshot> scene, QWidget* parent)
    : QDialog(parent), offline_image_(std::move(image)), offline_scene_(std::move(scene)) {
    build();
    setWindowTitle("离线降噪");
    prepare_controls();
}

SequenceViewer::~SequenceViewer() {
    cancel_.request();
}

void SequenceViewer::build() {
    setObjectName("sequence_viewer");
    setAttribute(Qt::WA_DeleteOnClose);
    resize(720, 540);
    auto* layout = new QVBoxLayout(this);
    auto* tools  = new QHBoxLayout;
    layout->addLayout(tools);
    filter_ = new QComboBox;
    filter_->setObjectName("denoise_method");
    filter_->setEnabled(false);
    auto* split = new QCheckBox("与原图对照");
    save_       = new QPushButton("导出…");
    save_->setEnabled(false);
    tools->addWidget(filter_);
    tools->addWidget(split);
    tools->addStretch();
    tools->addWidget(save_);
    if (!entry_.videos.empty()) {
        auto* videos = new QToolButton;
        videos->setText("课程视频");
        videos->setPopupMode(QToolButton::InstantPopup);
        auto* menu = new QMenu(videos);
        videos->setMenu(menu);
        tools->insertWidget(2, videos);
        for (const auto& [name, file] : entry_.videos) {
            connect(menu->addAction(name), &QAction::triggered, this, [this, file] {
                workbench_detail::run_background(
                    this,
                    [root = entry_.root, file] {
                        catalog::verify(root, file);
                        return catalog::resolve(root, file.path);
                    },
                    [this](const auto& result) {
                        if (!result.error.empty()) {
                            status_->setText(QString::fromStdString(result.error));
                        } else if (!QDesktopServices::openUrl(QUrl::fromLocalFile(result.value))) {
                            status_->setText("未找到可播放此视频的应用");
                        }
                    });
            });
        }
    }
    auto* toggle = new QPushButton("滤波参数");
    toggle->setCheckable(true);
    tools->insertWidget(2, toggle);
    auto* options = new QWidget;
    auto* form    = new QFormLayout(options);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    layout->addWidget(options);
    options->hide();
    connect(toggle, &QPushButton::toggled, options, &QWidget::setVisible);
    const auto integer = [&](const QString& label, int DenoiseSettings::* member, int last) {
        auto* box = new QSpinBox;
        box->setRange(1, last);
        box->setValue(settings_.*member);
        box->setKeyboardTracking(false);
        form->addRow(label, box);
        connect(box, &QSpinBox::valueChanged, this, [this, member](int v) {
            settings_.*member = v;
            ++revision_;
            request_frame();
        });
    };
    const auto scalar = [&](const QString& label, float DenoiseSettings::* member, double low,
                            double high, double step) {
        auto* box = new QDoubleSpinBox;
        box->setDecimals(3);
        box->setRange(low, high);
        box->setSingleStep(step);
        box->setValue(settings_.*member);
        box->setKeyboardTracking(false);
        form->addRow(label, box);
        connect(box, &QDoubleSpinBox::valueChanged, this, [this, member](double v) {
            settings_.*member = float(v);
            ++revision_;
            request_frame();
        });
    };
    integer("双边半径 / 像素", &DenoiseSettings::radius, 12);
    integer("À-Trous 层数", &DenoiseSettings::iterations, 5);
    scalar("颜色标准差", &DenoiseSettings::color_sigma, .01, 10, .05);
    scalar("平面距离 / m", &DenoiseSettings::position_sigma, .001, 10, .01);
    scalar("法线权重指数", &DenoiseSettings::normal_power, 0, 128, 2);
    scalar("历史混合权重", &DenoiseSettings::history_weight, 0, 1, .05);
    scalar("历史限幅 / σ", &DenoiseSettings::clamp_sigma, 0, 8, .25);
    auto* cancel = new QPushButton("取消处理");
    tools->insertWidget(3, cancel);
    connect(cancel, &QPushButton::clicked, this, [this] {
        play_->setChecked(false);
        cancel_.request();
    });
    image_ = new ComparisonImage(this);
    layout->addWidget(image_, 1);
    status_ = new QLabel("正在读取输入…");
    status_->setObjectName("frame_status");
    layout->addWidget(status_);
    progress_ = new QProgressBar;
    progress_->setRange(0, 100);
    progress_->hide();
    layout->addWidget(progress_);
    auto* bar = new QHBoxLayout;
    layout->addLayout(bar);
    play_ = new QPushButton("播放");
    play_->setCheckable(true);
    play_->setEnabled(false);
    slider_ = new QSlider(Qt::Horizontal);
    slider_->setEnabled(false);
    bar->addWidget(play_);
    bar->addWidget(slider_, 1);
    play_timer_ = new QTimer(this);
    play_timer_->setInterval(50);
    connect(play_, &QPushButton::toggled, this, [this](bool active) {
        play_->setText(active ? "暂停" : "播放");
        if (active) {
            play_timer_->start();
        } else {
            play_timer_->stop();
        }
    });
    connect(play_timer_, &QTimer::timeout, this, [this] {
        if (!loading_ && !frames_.empty()) {
            slider_->setValue((slider_->value() + 1) % frames_.size());
        }
    });
    connect(split, &QCheckBox::toggled, this, [this](bool on) { image_->compare(on); });
    connect(save_, &QPushButton::clicked, this, [this] { save(); });
    auto* clock = new QTimer(this);
    clock->setInterval(100);
    connect(clock, &QTimer::timeout, this, [this] {
        if (loading_ && percent_) {
            progress_->setValue(percent_->load());
        }
    });
    clock->start();
}

void SequenceViewer::prepare_controls() {
    filter_->addItem("原始 beauty", 0);
    const bool guides = offline_scene_ || (!frames_.empty() && frames_.front().guides);
    if (guides) {
        filter_->addItem("联合双边", 1);
        filter_->addItem("À-Trous", 2);
        if (!offline_scene_) {
            filter_->addItem("时间累积", 3);
        }
    }
    if (!frames_.empty() && frames_.front().reference) {
        filter_->addItem(
            frames_.front().reference->path.endsWith(".exr") ? "高采样参考" : "课程参考视频帧", 4);
    }
    if (offline_scene_) {
        filter_->setCurrentIndex(2);
        slider_->hide();
        play_->hide();
    } else {
        slider_->setRange(0, static_cast<int>(frames_.size() - 1));
        slider_->setEnabled(true);
        play_->setEnabled(frames_.size() > 1);
    }
    filter_->setEnabled(true);
    connect(filter_, &QComboBox::currentIndexChanged, this, [this] {
        ++revision_;
        request_frame();
    });
    connect(slider_, &QSlider::valueChanged, this, [this] {
        ++revision_;
        request_frame();
    });
    request_frame();
}

void SequenceViewer::request_frame() {
    if (loading_) {
        cancel_.request();
        return;
    }
    if (!offline_scene_ && frames_.empty()) {
        return;
    }
    loading_           = true;
    cancel_            = CancellationSource();
    percent_           = std::make_shared<std::atomic<int>>(0);
    const int  version = revision_, index = slider_->value(),
               method   = filter_->currentData().toInt();
    const auto frame    = offline_scene_ ? catalog::Frame{} : frames_.at(index);
    const auto previous = current_;
    // 跨帧或切换方法后的历史只能在时间模式的相邻帧间复用。
    const bool same = previous && previous->index == index;
    const bool continuous =
        previous && previous->method == 3 && method == 3 && previous->index + 1 == index;
    status_->setText(QString("正在处理第 %1 帧…").arg(index + 1));
    progress_->setValue(0);
    progress_->show();
    save_->setEnabled(false);
    workbench_detail::run_background(
        this,
        [root    = entry_.root, frame, index, method, previous, same, continuous,
         offline = offline_scene_, provider = offline_image_, settings = settings_,
         token = cancel_.token(), progress = percent_] {
            auto out      = std::make_shared<ViewedFrame>();
            out->index    = index;
            out->method   = method;
            out->original = same      ? previous->original
                            : offline ? provider()
                                      : std::make_shared<const Image>(
                                            Image::read_exr(checked(root, frame.beauty, token)));
            if (!out->original) {
                throw std::runtime_error("离线任务尚无可读图像");
            }
            if (same) {
                out->guides = previous->guides;
            }
            auto report = [progress](auto done, auto total) {
                if (total) {
                    progress->store(int(100 * done / total));
                }
            };
            if (method >= 1 && method <= 3) {
                if (!out->guides) {
                    if (offline) {
                        out->guides = std::make_shared<DenoiseGuides>(
                            make_guides(offline, out->original->width(), out->original->height(),
                                        token, report));
                    } else {
                        if (!frame.guides) {
                            throw std::runtime_error("该帧缺少降噪引导文件");
                        }
                        const auto& g = *frame.guides;
                        GuideFiles  paths{checked(root, g[0], token), checked(root, g[1], token),
                                          checked(root, g[2], token), checked(root, g[3], token),
                                          checked(root, g[4], token)};
                        out->guides = std::make_shared<DenoiseGuides>(read_guides(paths, token));
                    }
                }
                auto s     = settings;
                s.filter   = method == 1 ? SpatialFilter::bilateral : SpatialFilter::atrous;
                auto image = filter_spatial(*out->original, *out->guides, s, token, report);
                if (continuous) {
                    auto temporal = filter_temporal(image, *out->guides, *previous->filtered,
                                                    *previous->guides, s, token, report);
                    image         = std::move(temporal.image);
                    out->accepted = temporal.accepted;
                }
                out->filtered = std::make_shared<const Image>(std::move(image));
            } else if (method == 4) {
                if (!frame.reference) {
                    throw std::runtime_error("缺少参考帧");
                }
                const auto file = checked(root, *frame.reference, token);
                out->filtered   = std::make_shared<const Image>(
                    Image::read(file, file.extension() == ".exr" ? ColorEncoding::linear
                                                                 : ColorEncoding::srgb));
            } else {
                out->filtered = out->original;
            }
            token.check();
            out->before = same ? previous->before : display(*out->original, bool(offline));
            out->after  = method == 0 ? out->before : display(*out->filtered, bool(offline));
            return out;
        },
        [this, version](const auto& result) {
            loading_ = false;
            progress_->hide();
            if (version != revision_) {
                request_frame();
                return;
            }
            if (!result.error.empty()) {
                play_->setChecked(false);
                save_->setEnabled(bool(current_));
                status_->setText(cancel_.token().requested()
                                     ? "处理已取消"
                                     : QString::fromStdString(result.error));
                return;
            }
            current_ = result.value;
            image_->images(current_->before, current_->after);
            save_->setEnabled(true);
            auto text = QString("%1 / %2   ·   %3 × %4")
                            .arg(current_->index + 1)
                            .arg(offline_scene_ ? 1 : frames_.size())
                            .arg(current_->after.width())
                            .arg(current_->after.height());
            if (current_->method == 3) {
                text += QString("   ·   复用历史 %1 像素").arg(current_->accepted);
            }
            status_->setText(text);
        });
}

void SequenceViewer::save() {
    if (!current_) {
        return;
    }
    const auto file = QFileDialog::getSaveFileName(this, "导出当前结果", {},
                                                   "Linear EXR (*.exr);;Display PNG (*.png)");
    if (file.isEmpty()) {
        return;
    }
    workbench_detail::run_background(
        this,
        [result = current_, file] {
            if (file.endsWith(".png", Qt::CaseInsensitive)) {
                if (!result->after.save(file)) {
                    throw std::runtime_error("无法保存 PNG");
                }
            } else {
                result->filtered->write_exr(workbench_detail::native_path(file));
            }
            return file;
        },
        [this](const auto& result) {
            status_->setText(result.error.empty() ? "已保存 " + result.value
                                                  : QString::fromStdString(result.error));
        });
}
