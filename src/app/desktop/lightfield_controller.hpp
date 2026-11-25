// 光场采集和重建任务、文件操作及预览来源协调。

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
    // 固定场景快照和采集设置，启动后台光场采集。
    void start(const std::function<std::shared_ptr<const minirender::SceneSnapshot>()>& snapshot,
               minirender::LightFieldCaptureSettings                                    settings,
               minirender::IntegratorSettings                                           integrator);
    // 在暂停与恢复采集之间切换，保持同一任务的数据。
    void pause_resume();
    // 取消采集或当前图像任务，丢弃未完成结果。
    void cancel();
    // 标记已有采集快照或结果与当前场景不一致。
    void scene_changed();
    // 后台打开完整光场文件，首张重建成功后再发布新数据。
    void open(const QString& path, std::uint64_t memory, std::uint64_t cache);
    // 后台复制当前完整光场文件，完成后更新状态消息。
    void save(const QString& path);
    // 导出完整重建图像；EXR 保留线性值，PNG 使用当前显示曝光。
    void export_image(const QString& path, bool exr);
    void reconstruct(double focus, double aperture, std::optional<std::size_t> view = {});
    // 切换光场图像的显示归属，关闭时恢复场景预览。
    void show(bool visible);
    // 停止接收请求并等待后台工作退出，供窗口销毁调用。
    void shutdown();
    // 查询采集任务是否仍占用离线计算入口。
    bool capture_active() const;

    // 查询是否处于采集及完成结果接收过程。
    bool acquiring() const noexcept {
        return acquiring_;
    }

    // 返回当前图像或文件后台任务类型。
    Job job() const noexcept {
        return job_;
    }

    // 返回采集会话的只读引用，供面板读取进度。
    const auto& capture() const noexcept {
        return capture_;
    }

    // 返回当前完整光场文件的共享所有权引用。
    const auto& file() const noexcept {
        return file_;
    }

    // 返回完整重建图像的共享只读引用。
    const auto& image() const noexcept {
        return image_;
    }

    // 返回当前可显示的任务状态或错误说明。
    QString message() const {
        return message_;
    }

    // 判断正在采集的快照或已显示结果是否落后于场景修改。
    bool stale() const noexcept {
        return acquiring_ ? capture_stale_ : data_stale_;
    }

    // 返回当前图像任务的完成数量和总工作量。
    std::pair<std::uint64_t, std::uint64_t> work() const;

    // 返回已发布光场数据版本。
    std::uint64_t data_revision() const noexcept {
        return data_revision_;
    }

    // 返回本次孔径选择实际参与重建的视图数。
    std::size_t selected_views() const noexcept {
        return selected_views_;
    }

    std::function<bool()> other_render_active;

  private:
    using ImagePtr  = std::shared_ptr<const minirender::Image>;
    using FilePtr   = std::shared_ptr<const minirender::LightFieldFile>;
    using ReaderPtr = std::shared_ptr<minirender::LightFieldReader>;

    // 固定本次焦距、孔径和可选子视图编号。
    struct Request {
        double                     focus;
        double                     aperture;
        std::optional<std::size_t> view;
    };

    // 保存工作线程生成的文件、读取器或图像，完成后由 GUI 接收。
    struct Result {
        FilePtr     file;
        ReaderPtr   reader;
        ImagePtr    image;
        ImagePtr    thumbnail;
        std::size_t views = 0;
        QString     error;
        bool        cancelled = false;
    };

    // 工作线程用原子计数更新完成量，面板按定时器间隔采样。
    struct Progress {
        std::atomic<std::uint64_t> done{0}, total{0};
    };

    using Task =
        std::function<Result(minirender::CancellationToken, minirender::LightFieldCallback)>;
    // 启动一个独占 reader 的后台任务，统一处理取消与结果发布。
    void launch(Job job, Task task);
    // 准备候选文件及首次重建，成功前保留原有文件和显示图像。
    void initialize(FilePtr file, QString path, Job job);
    // 把固定的光学请求交给后台重建或子视图读取。
    void begin_request(Request request);
    // 在 GUI 线程接收已结束任务，并按显示版本决定是否更新前台。
    void finish();
    // 轮询采集进度和结果，提交必要的预览缩略图任务。
    void poll();
    // 检查文件或图像操作所需的空闲状态，不满足时抛出异常。
    void require_idle() const;
    // 借用视口，用于发布图像和协调显示来源。
    VulkanViewport& viewport_;
    // 独占的后台光场采集会话。
    minirender::LightFieldCaptureSession capture_;
    // 当前已发布且完整的光场容器。
    FilePtr file_;
    // 被图像工作线程独占使用的视图缓存。
    ReaderPtr reader_;
    // 完整重建图像，导出使用该分辨率。
    ImagePtr image_;
    // 用于视口上传的缩小预览图。
    ImagePtr thumbnail_;
    // 当前采集过程的最近子视图缩略图。
    ImagePtr capture_thumbnail_;
    // 合并后的最新光学请求，当前工作退出后执行。
    std::optional<Request> pending_;
    // 把后台任务完成事件投递回 GUI 线程。
    QFutureWatcher<Result>* watcher_;
    // 当前后台图像或文件操作的类型。
    Job job_ = Job::idle;
    // 当前后台图像任务使用的取消状态。
    minirender::CancellationSource cancellation_;
    // 由 worker 更新、GUI 读取的共享计数。
    std::shared_ptr<Progress> progress_;
    // 当前重建与缓存共用的工作内存预算。
    std::uint64_t memory_           = 256ULL << 20;
    std::uint64_t candidate_memory_ = 0, candidate_cache_ = 0, data_revision_ = 0,
                  open_revision_ = 0;
    // 最近重建采用的有效子视图数。
    std::size_t selected_views_ = 0;
    // 最近已经处理的采集预览版本。
    std::size_t seen_preview_ = 0;
    bool discard_capture_ = false, acquiring_ = false, capture_stale_ = false, data_stale_ = false,
         scene_linked_ = false, stopped_ = false;
    // 面板显示的完成或错误信息。
    QString message_;
};
