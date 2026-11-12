#pragma once

#include "vulkan_viewport.hpp"
#include <QFutureWatcher>
#include <QObject>
#include <atomic>
#include <minirender/render/lightfield_capture.hpp>
#include <optional>

// GUI 线程负责发布状态；单个图像任务独占读者缓存。后台计算不访问控件或 Vulkan。
class LightFieldController final : public QObject {
  public:
    enum class Job { idle, load, focus, view, save, image_export, thumbnail, adopt };
    explicit LightFieldController(VulkanViewport& viewport, QObject* parent = nullptr);
    ~LightFieldController() override;
    void start(const std::function<std::shared_ptr<const minirender::SceneSnapshot>()>& snapshot,
               minirender::LightFieldCaptureSettings settings,
               minirender::IntegratorSettings integrator);
    void pause_resume();
    void cancel();
    void scene_changed();
    void open(const QString& path, std::uint64_t memory, std::uint64_t cache);
    void save(const QString& path);
    void export_image(const QString& path, bool exr);
    void reconstruct(double focus, double aperture, std::optional<std::size_t> view = {});
    void show(bool visible);
    void shutdown();
    bool capture_active() const;
    bool acquiring() const noexcept {
        return acquiring_;
    }
    Job job() const noexcept {
        return job_;
    }
    const auto& capture() const noexcept {
        return capture_;
    }
    const auto& file() const noexcept {
        return file_;
    }
    const auto& image() const noexcept {
        return image_;
    }
    QString message() const {
        return message_;
    }
    bool stale() const noexcept {
        return acquiring_ ? capture_stale_ : data_stale_;
    }
    std::pair<std::uint64_t, std::uint64_t> work() const;
    std::uint64_t data_revision() const noexcept {
        return data_revision_;
    }
    std::size_t selected_views() const noexcept {
        return selected_views_;
    }
    std::function<bool()> other_render_active;

  private:
    using ImagePtr = std::shared_ptr<const minirender::Image>;
    using FilePtr = std::shared_ptr<const minirender::LightFieldFile>;
    using ReaderPtr = std::shared_ptr<minirender::LightFieldReader>;
    struct Request {
        double focus, aperture;
        std::optional<std::size_t> view;
    };
    struct Result {
        FilePtr file;
        ReaderPtr reader;
        ImagePtr image, thumbnail;
        std::size_t views = 0;
        QString error;
        bool cancelled = false;
    };
    struct Progress {
        std::atomic<std::uint64_t> done{0}, total{0};
    };
    using Task =
        std::function<Result(minirender::CancellationToken, minirender::LightFieldCallback)>;
    void launch(Job job, Task task);
    void initialize(FilePtr file, QString path, Job job);
    void begin_request(Request request);
    void finish();
    void poll();
    void require_idle() const;
    VulkanViewport& viewport_;
    minirender::LightFieldCaptureSession capture_;
    FilePtr file_;
    ReaderPtr reader_;
    ImagePtr image_, thumbnail_, capture_thumbnail_;
    std::optional<Request> pending_;
    QFutureWatcher<Result>* watcher_;
    Job job_ = Job::idle;
    minirender::CancellationSource cancellation_;
    std::shared_ptr<Progress> progress_;
    std::uint64_t memory_ = 256ULL << 20;
    std::uint64_t candidate_memory_ = 0, candidate_cache_ = 0, data_revision_ = 0,
                  open_revision_ = 0;
    std::size_t selected_views_ = 0, seen_preview_ = 0;
    bool discard_capture_ = false, acquiring_ = false, capture_stale_ = false, data_stale_ = false,
         scene_linked_ = false, stopped_ = false;
    QString message_;
};
