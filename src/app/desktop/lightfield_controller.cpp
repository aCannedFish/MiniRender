#include "lightfield_controller.hpp"
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <stdexcept>

namespace {
using namespace minirender;
using ImagePtr = std::shared_ptr<const Image>;

// 保留完整线性图用于导出，传给视口的图像限制为1024像素长边。
ImagePtr preview(ImagePtr input, CancellationToken token) {
    const auto longest = std::max(input->width(), input->height());
    if (longest <= 1024) {
        return input;
    }
    auto image =
        std::make_shared<Image>(std::max<std::size_t>(1, input->width() * 1024 / longest),
                                std::max<std::size_t>(1, input->height() * 1024 / longest));
    for (std::size_t y = 0; y < image->height(); ++y) {
        token.check();
        for (std::size_t x = 0; x < image->width(); ++x) {
            image->at(x, y) = input->at(x * input->width() / image->width(),
                                        y * input->height() / image->height());
        }
    }
    return image;
}
} // namespace
LightFieldController::LightFieldController(VulkanViewport& viewport, QObject* parent)
    : QObject(parent), viewport_(viewport), watcher_(new QFutureWatcher<Result>(this)) {
    connect(watcher_, &QFutureWatcher<Result>::finished, this, [this] { finish(); });
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] { poll(); });
    timer->start(80);
}
LightFieldController::~LightFieldController() {
    shutdown();
}
void LightFieldController::shutdown() {
    if (stopped_) {
        return;
    }
    stopped_ = true;
    pending_.reset();
    cancellation_.request();
    capture_.cancel();
    capture_.wait();
    watcher_->waitForFinished();
}
bool LightFieldController::capture_active() const {
    return !render_terminal(capture_.progress().state);
}
void LightFieldController::require_idle() const {
    if (acquiring_ || job_ != Job::idle) {
        throw std::logic_error("请等待当前光场任务完成或先取消。");
    }
}
void LightFieldController::start(
    const std::function<std::shared_ptr<const SceneSnapshot>()>& snapshot,
    LightFieldCaptureSettings settings, IntegratorSettings integrator) {
    require_idle();
    if (other_render_active && other_render_active()) {
        throw std::logic_error("请先结束离线渲染或导出，再采集光场。");
    }
    if (lightfield_refocus_scratch_bytes(settings.render) > settings.memory_budget_bytes) {
        throw std::length_error("光场重建超过内存预算。");
    }
    capture_.start(snapshot(), settings, integrator);
    capture_thumbnail_.reset();
    discard_capture_ = false;
    acquiring_ = true;
    capture_stale_ = false;
    seen_preview_ = 0;
    candidate_memory_ = settings.memory_budget_bytes;
    candidate_cache_ = std::min<std::uint64_t>(64ULL << 20, candidate_memory_ / 4);
    message_ = "正在采集光场";
    show(true);
}
void LightFieldController::pause_resume() {
    const auto state = capture_.progress().state;
    if (state == RenderState::paused || state == RenderState::pausing) {
        capture_.resume();
    } else {
        capture_.pause();
    }
}
void LightFieldController::cancel() {
    discard_capture_ = acquiring_;
    pending_.reset();
    cancellation_.request();
    capture_.cancel();
}
void LightFieldController::scene_changed() {
    if (acquiring_) {
        capture_stale_ = true;
    }
    if (scene_linked_) {
        data_stale_ = true;
    }
}
void LightFieldController::show(bool visible) {
    if (visible) {
        viewport_.set_offline_image(acquiring_ && capture_thumbnail_ ? capture_thumbnail_
                                                                     : thumbnail_,
                                    OfflineImageSource::lightfield);
        viewport_.set_offline_visible(true);
    } else if (viewport_.offline_source() == OfflineImageSource::lightfield) {
        viewport_.set_offline_visible(false);
    }
}
std::pair<std::uint64_t, std::uint64_t> LightFieldController::work() const {
    return progress_ ? std::pair{progress_->done.load(), progress_->total.load()}
                     : std::pair<std::uint64_t, std::uint64_t>{};
}
void LightFieldController::launch(Job job, Task task) {
    cancellation_ = CancellationSource();
    progress_ = std::make_shared<Progress>();
    job_ = job;
    watcher_->setFuture(QtConcurrent::run(
        [task = std::move(task), token = cancellation_.token(), progress = progress_] {
            Result result;
            try {
                result = task(token, [&](LightFieldWork work) {
                    progress->total.store(work.total);
                    progress->done.store(work.completed);
                });
            } catch (const OperationCancelled&) {
                result.cancelled = true;
            } catch (const std::exception& e) {
                result.error = QString::fromUtf8(e.what());
            }
            return result;
        }));
}
// 首次重建成功后才替换数据。损坏文件和预算错误均保留原结果。
void LightFieldController::initialize(FilePtr file, QString path, Job job) {
    launch(job, [file = std::move(file), path = std::move(path), memory = candidate_memory_,
                 cache = candidate_cache_](auto token, auto progress) {
        Result r;
        r.file =
            file ? file : LightFieldFile::open(std::filesystem::path(path.toStdU16String()), token);
        const auto scratch = lightfield_refocus_scratch_bytes(r.file->calibration().render);
        if (scratch > memory) {
            throw std::length_error("光场重建超过内存预算。");
        }
        r.reader = std::make_shared<LightFieldReader>(r.file, std::min(cache, memory - scratch));
        const auto& c = r.file->calibration();
        auto result = refocus_lightfield(*r.reader, {c.reference_focus, c.pupil_radius, memory},
                                         token, progress);
        r.views = result.selected_views;
        r.image = std::make_shared<const Image>(std::move(result.image));
        r.thumbnail = preview(r.image, token);
        return r;
    });
}
void LightFieldController::open(const QString& path, std::uint64_t memory, std::uint64_t cache) {
    require_idle();
    candidate_memory_ = memory;
    candidate_cache_ = cache;
    open_revision_ = viewport_.display_revision();
    message_ = "正在读取并重建光场";
    initialize({}, path, Job::load);
}
void LightFieldController::save(const QString& path) {
    require_idle();
    if (!file_) {
        throw std::logic_error("尚无完整光场。");
    }
    launch(Job::save, [file = file_, path](auto token, auto progress) {
        file->save(std::filesystem::path(path.toStdU16String()), token, progress);
        return Result{};
    });
}
void LightFieldController::export_image(const QString& path, bool exr) {
    require_idle();
    if (!image_) {
        throw std::logic_error("尚无光场图像。");
    }
    launch(Job::image_export,
           [image = image_, path, exr, ev = viewport_.exposure()](auto token, auto) {
               token.check();
               const auto output = std::filesystem::path(path.toStdU16String());
               if (exr) {
                   image->write_exr(output);
               } else {
                   display_image(*image, ev).write_png(output);
               }
               return Result{};
           });
}
void LightFieldController::reconstruct(double focus, double aperture,
                                       std::optional<std::size_t> view) {
    if (!file_ || acquiring_ || (job_ != Job::idle && job_ != Job::focus && job_ != Job::view)) {
        throw std::logic_error("当前没有可调整的光场。");
    }
    if (!viewport_.offline_visible() ||
        viewport_.offline_source() != OfflineImageSource::lightfield) {
        show(true);
    }
    Request request{focus, aperture, view};
    if (job_ != Job::idle) {
        pending_ = request;
        cancellation_.request();
    } else {
        begin_request(request);
    }
}
void LightFieldController::begin_request(Request request) {
    launch(request.view ? Job::view : Job::focus,
           [reader = reader_, request, memory = memory_](auto token, auto progress) {
               Result r;
               if (request.view) {
                   r.image = reader->view(*request.view, token);
                   r.views = 1;
               } else {
                   auto result = refocus_lightfield(
                       *reader, {request.focus, request.aperture, memory}, token, progress);
                   r.views = result.selected_views;
                   r.image = std::make_shared<const Image>(std::move(result.image));
               }
               r.thumbnail = preview(r.image, token);
               return r;
           });
}
void LightFieldController::finish() {
    if (stopped_) {
        return;
    }
    auto result = watcher_->result();
    const auto completed = job_;
    job_ = Job::idle;
    if (pending_) {
        auto request = *pending_;
        pending_.reset();
        begin_request(request);
        return;
    }
    if (completed == Job::adopt) {
        acquiring_ = false;
        capture_thumbnail_.reset();
        if ((!result.error.isEmpty() || result.cancelled || cancellation_.token().requested()) &&
            viewport_.offline_source() == OfflineImageSource::lightfield) {
            viewport_.set_offline_image(thumbnail_, OfflineImageSource::lightfield);
        }
    }
    if (!result.error.isEmpty()) {
        message_ = result.error;
        return;
    }
    if (result.cancelled || (cancellation_.token().requested() && completed != Job::save &&
                             completed != Job::image_export)) {
        message_ = "已取消";
        return;
    }
    if (completed == Job::thumbnail) {
        capture_thumbnail_ = std::move(result.thumbnail);
        if (viewport_.offline_visible() &&
            viewport_.offline_source() == OfflineImageSource::lightfield) {
            viewport_.set_offline_image(capture_thumbnail_, OfflineImageSource::lightfield);
        }
        return;
    }
    // 更换文件后在后台释放旧 LRU，避免在事件线程析构整批大图。
    if (result.file && reader_) {
        QThreadPool::globalInstance()->start(
            [old = std::move(reader_), storage = std::move(file_)]() mutable {
                old.reset();
                storage.reset();
            });
    }
    if (result.image && image_ && std::max(image_->width(), image_->height()) > 1024) {
        QThreadPool::globalInstance()->start([old = std::move(image_)]() mutable { old.reset(); });
    }
    if (result.file) {
        file_ = std::move(result.file);
        reader_ = std::move(result.reader);
        memory_ = candidate_memory_;

        scene_linked_ = completed == Job::adopt;
        data_stale_ = scene_linked_ && capture_stale_;
        ++data_revision_;
    }
    if (result.image) {
        image_ = std::move(result.image);
        thumbnail_ = std::move(result.thumbnail);
        selected_views_ = result.views;
        if (completed == Job::load && open_revision_ == viewport_.display_revision()) {
            show(true);
        } else if (viewport_.offline_visible() &&
                   viewport_.offline_source() == OfflineImageSource::lightfield) {
            viewport_.set_offline_image(thumbnail_, OfflineImageSource::lightfield);
        }
    }
    message_ = completed == Job::save           ? "光场已保存"
               : completed == Job::image_export ? "图像已导出"
               : completed == Job::thumbnail    ? "正在采集光场"
               : completed == Job::view         ? "已显示子视图"
                                                : "重聚焦完成";
}
void LightFieldController::poll() {
    if (stopped_ || !acquiring_ || job_ != Job::idle) {
        return;
    }
    const auto p = capture_.progress();
    if (p.state == RenderState::completed && !discard_capture_) {
        initialize(capture_.result(), {}, Job::adopt);
        return;
    }
    if (render_terminal(p.state)) {
        acquiring_ = false;
        capture_thumbnail_.reset();
        if (viewport_.offline_source() == OfflineImageSource::lightfield) {
            viewport_.set_offline_image(thumbnail_, OfflineImageSource::lightfield);
        }
        message_ = p.error.empty() ? "采集已取消" : QString::fromStdString(p.error);
        return;
    }
    const auto raw = capture_.preview();
    if (raw.image && raw.completed_views > seen_preview_) {
        seen_preview_ = raw.completed_views;
        launch(Job::thumbnail, [image = raw.image](auto token, auto) {
            Result r;
            r.thumbnail = preview(image, token);
            r.views = 1;
            return r;
        });
    }
}
